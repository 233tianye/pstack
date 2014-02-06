#include <unistd.h>
#include <assert.h>
#include <iostream>
#include <exception>
#include <algorithm>
#include <memory>
#include "procinfo.h"
#include "elfinfo.h"
#include "dwarf.h"

using namespace std;

extern "C" {
#include "proc_service.h"
}

static int
globmatchR(const char *pattern, const char *name)
{
    for (;; name++) {
        switch (*pattern) {
        case '*':
            // if the rest of the name matches the bit of pattern after '*',
            for (;;) {
                ++name;
                if (globmatchR(pattern + 1, name))
                    return 1;
                if (*name == 0) // exhuasted name without finding a match
                    return 0;
            }
        default:
            if (*name != *pattern)
                return 0;
        }
        if (*pattern++ == 0)
            return 1;
    }
}

static int
globmatch(string pattern, string name)
{
    return globmatchR(pattern.c_str(), name.c_str());
}


struct ListedSymbol {
    Elf_Sym sym;
    Elf_Off objbase;
    string objname;
    size_t count;
    string name;
    ListedSymbol(const Elf_Sym &sym_, Elf_Off objbase_, string name_, string object)
        : sym(sym_)
        , objbase(objbase_)
        , name(name_)
        , count(0)
        , objname(object)

    {
    }
    Elf_Off memaddr() const { return  sym.st_value + objbase; }
};

const bool operator < (const ListedSymbol &sym, Elf_Off addr) {
    return sym.memaddr() + sym.sym.st_size < addr;
}

struct Symcounter {
    Elf_Off addr;
    string name;
    unsigned count;
    struct ListedSymbol *sym;
};

vector<Symcounter> counters;

#ifdef __sun__
static const char *default_pattern = "*__vtbl_"; /* SunOS compiler */
#else
static const char *default_pattern = "_ZTV*"; /* GCC */
#endif

static vector<const char *>virtpatterns;

int
mainExcept(int argc, char *argv[])
{
    bool findRef = false; 
    shared_ptr<ElfObject> exec;
    shared_ptr<ElfObject> core;
    shared_ptr<Process> process;
    int c;
    int verbose = 0;
    bool showaddrs = false;

    Elf_Off minval, maxval;
    char *strbuf = 0;
    char *findstr = 0;
    size_t findstrlen;

    while ((c = getopt(argc, argv, "vhsp:f:e:S:")) != -1) {
        switch (c) {
            case 'p':
                virtpatterns.push_back(optarg);
                break;
            case 's':
                showaddrs = true;
                break;
            case 'v':
                debug = &clog;
                verbose++;
                break;
            case 'h':
                clog << "usage: canal [exec] <core>" << endl;
                return 0;
            case 'S':
                findstr = optarg;
                findstrlen = strlen(findstr);
                strbuf = new char[findstrlen];
                break;

            case 'f':
                findRef = true;
                maxval = minval = strtoll(optarg, 0, 0);
                break;

            case 'e':
                if (!findRef)
                    abort();
                maxval = strtoll(optarg, 0, 0);
                break;

            case 'X':
                ps_lgetfpregs(0, 0, 0);
        }
    }

    if (argc - optind >= 2) {
        exec = make_shared<ElfObject>(argv[optind]);
        optind++;
    }

    if (argc - optind >= 1) {
        char *eoa;
        pid_t pid = strtol(argv[optind], &eoa, 10);
        core = make_shared<ElfObject>(argv[optind]);
        process = make_shared<CoreProcess>(exec, core);
    }
    process->load();
    if (findRef) {
        std::clog << "finding references to addresses from " << hex << minval << " to " << maxval << "\n";
    }
    clog << "opened process " << process << endl;

    vector<ListedSymbol> listed;
    if (virtpatterns.size() == 0)
        virtpatterns.push_back(default_pattern);
    for (auto &loaded : process->objects) {
        size_t count = 0;

        for (const char * name : { ".dynsym", ".symtab" }) {
            for (const auto sym : loaded.object->getSymbols(name)) {
                for (const auto &virtpattern : virtpatterns) {
                    if (globmatch(virtpattern, sym.second)) {
                        listed.push_back(ListedSymbol(sym.first, loaded.reloc, sym.second, loaded.object->io->describe()));
                        if (verbose)
                            clog << "added symbol " << sym.second << endl;
                        count++;
                        break;
                    }
                }
            }
        }
        if (debug)
            *debug << "found " << count << " symbols in " << loaded.object->io->describe() << endl;
    }
    sort(listed.begin()
        , listed.end()
        , [] (const ListedSymbol &l, const ListedSymbol &r) { return l.memaddr() < r.memaddr(); });

    // Now run through the corefile, searching for virtual objects.
    off_t filesize = 0;
    off_t memsize = 0;
    for (auto hdr : core->getSegments()) {
        if (hdr.p_type != PT_LOAD)
            continue;
        Elf_Off p;
        filesize += hdr.p_filesz;
        memsize += hdr.p_memsz;
        if (debug) {
            *debug << "scan " << hex << hdr.p_vaddr <<  " to " << hdr.p_vaddr + hdr.p_memsz << " ";
            *debug << "(filesiz = " << hdr.p_filesz  << ", memsiz=" << hdr.p_memsz << ") ";
        }

        if (findstr) {
            for (auto loc = hdr.p_vaddr; loc < hdr.p_vaddr + hdr.p_filesz - findstrlen; loc++) {
                size_t rc = process->io->read(loc, findstrlen, strbuf);
                if (memcmp(strbuf, findstr, findstrlen) == 0)
                    std::cout << "0x" << hex << loc << "\n";
            }
        } else {
            static const size_t pagesize = getpagesize();
            union Page {
                char *chars;
                void **pointers;
            };
            Page page;
            page.chars = new char[pagesize];

            assert(hdr.p_vaddr % pagesize == 0);

            for (auto loc = hdr.p_vaddr; loc < hdr.p_vaddr + hdr.p_filesz; loc += pagesize) {
                if (verbose && (loc - hdr.p_vaddr) % (1024 * 1024) == 0)
                    clog << '.';
                process->io->readObj(loc, page.chars, pagesize);
                for (size_t i = 0; i < pagesize / sizeof (void *); ++i) {
                    intptr_t p = (intptr_t)page.pointers[i];
                    if (findRef) { 
                        if (p >= minval && p < maxval && (p % 4 == 0))
                            cout << "0x" << hex << loc << "\n";
                    } else {
                        auto found = lower_bound(listed.begin(), listed.end(), p);
                        if (found != listed.end() && found->memaddr() <= p && found->memaddr() + found->sym.st_size > p) {
                            if (showaddrs)
                                cout << found->name << " + " << p - found->memaddr() << " " << loc + i * sizeof (void *) << endl;
                            found->count++;
                        }
                    }
                }
            }
        }

        if (debug)
            *debug << endl;

    }
    if (debug)
        *debug << "core file contains " << filesize << " out of " << memsize << " bytes of memory\n";

    sort(listed.begin()
        , listed.end()
        , [] (const ListedSymbol &l, const ListedSymbol &r) { return l.count > r.count; });

    for (auto &i : listed)
        if (i.count)
            cout << dec << i.count << " " << i.name << " ( from " << i.objname << ")" << endl;
    return 0;
}

int
main(int argc, char *argv[])
{
    try {
        return mainExcept(argc, argv);
    }
    catch (const exception &ex) {
        cerr << "exception: " << ex.what() << endl;
        return -1;
    }
}
