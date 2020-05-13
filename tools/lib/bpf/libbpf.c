// SPDX-License-Identifier: LGPL-2.1

/*
 * Common eBPF ELF object loading operations.
 *
 * Copyright (C) 2013-2015 Alexei Starovoitov <ast@kernel.org>
 * Copyright (C) 2015 Wang Nan <wangnan0@huawei.com>
 * Copyright (C) 2015 Huawei Inc.
 * Copyright (C) 2017 Nicira, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License (not later!)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not,  see <http://www.gnu.org/licenses>
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <libgen.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <perf-sys.h>
#include <asm/unistd.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/list.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <tools/libc_compat.h>
#include <libelf.h>
#include <gelf.h>

#include "libbpf.h"
#include "bpf.h"
#include "btf.h"
#include "str_error.h"

#ifndef EM_BPF
#define EM_BPF 247
#endif

#ifndef BPF_FS_MAGIC
#define BPF_FS_MAGIC		0xcafe4a11
#endif

#define __printf(a, b)	__attribute__((format(printf, a, b)))

__printf(1, 2)
static int __base_pr(const char *format, ...)
{
	va_list args;
	int err;

	va_start(args, format);
	err = vfprintf(stderr, format, args);
	va_end(args);
	return err;
}

static __printf(1, 2) libbpf_print_fn_t __pr_warning = __base_pr;
static __printf(1, 2) libbpf_print_fn_t __pr_info = __base_pr;
static __printf(1, 2) libbpf_print_fn_t __pr_debug;

#define __pr(func, fmt, ...)	\
do {				\
	if ((func))		\
		(func)("libbpf: " fmt, ##__VA_ARGS__); \
} while (0)

#define pr_warning(fmt, ...)	__pr(__pr_warning, fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)	__pr(__pr_info, fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)	__pr(__pr_debug, fmt, ##__VA_ARGS__)

void libbpf_set_print(libbpf_print_fn_t warn,
		      libbpf_print_fn_t info,
		      libbpf_print_fn_t debug)
{
	__pr_warning = warn;
	__pr_info = info;
	__pr_debug = debug;
}

#define STRERR_BUFSIZE  128

#define CHECK_ERR(action, err, out) do {	\
	err = action;			\
	if (err)			\
		goto out;		\
} while(0)


/* Copied from tools/perf/util/util.h */
#ifndef zfree
# define zfree(ptr) ({ free(*ptr); *ptr = NULL; })
#endif

#ifndef zclose
# define zclose(fd) ({			\
	int ___err = 0;			\
	if ((fd) >= 0)			\
		___err = close((fd));	\
	fd = -1;			\
	___err; })
#endif

#ifdef HAVE_LIBELF_MMAP_SUPPORT
# define LIBBPF_ELF_C_READ_MMAP ELF_C_READ_MMAP
#else
# define LIBBPF_ELF_C_READ_MMAP ELF_C_READ
#endif

/*
 * bpf_prog should be a better name but it has been used in
 * linux/filter.h.
 */
struct bpf_program {
	/* Index in elf obj file, for relocation use. */
	int idx;
	char *name;
	int prog_ifindex;
	char *section_name;
	struct bpf_insn *insns;
	size_t insns_cnt, main_prog_cnt;
	enum bpf_prog_type type;

	struct reloc_desc {
		enum {
			RELO_LD64,
			RELO_CALL,
		} type;
		int insn_idx;
		union {
			int map_idx;
			int text_off;
		};
	} *reloc_desc;
	int nr_reloc;

	struct {
		int nr;
		int *fds;
	} instances;
	bpf_program_prep_t preprocessor;

	struct bpf_object *obj;
	void *priv;
	bpf_program_clear_priv_t clear_priv;

	enum bpf_attach_type expected_attach_type;
};

struct bpf_map {
	int fd;
	char *name;
	size_t offset;
	int map_ifindex;
	struct bpf_map_def def;
	__u32 btf_key_type_id;
	__u32 btf_value_type_id;
	void *priv;
	bpf_map_clear_priv_t clear_priv;
};

static LIST_HEAD(bpf_objects_list);

struct bpf_object {
	char license[64];
	u32 kern_version;

	struct bpf_program *programs;
	size_t nr_programs;
	struct bpf_map *maps;
	size_t nr_maps;

	bool loaded;
	bool has_pseudo_calls;

	/*
	 * Information when doing elf related work. Only valid if fd
	 * is valid.
	 */
	struct {
		int fd;
		void *obj_buf;
		size_t obj_buf_sz;
		Elf *elf;
		GElf_Ehdr ehdr;
		Elf_Data *symbols;
		size_t strtabidx;
		struct {
			GElf_Shdr shdr;
			Elf_Data *data;
		} *reloc;
		int nr_reloc;
		int maps_shndx;
		int text_shndx;
	} efile;
	/*
	 * All loaded bpf_object is linked in a list, which is
	 * hidden to caller. bpf_objects__<func> handlers deal with
	 * all objects.
	 */
	struct list_head list;

	struct btf *btf;

	void *priv;
	bpf_object_clear_priv_t clear_priv;

	char path[];
};
#define obj_elf_valid(o)	((o)->efile.elf)

static void bpf_program__unload(struct bpf_program *prog)
{
	int i;

	if (!prog)
		return;

	/*
	 * If the object is opened but the program was never loaded,
	 * it is possible that prog->instances.nr == -1.
	 */
	if (prog->instances.nr > 0) {
		for (i = 0; i < prog->instances.nr; i++)
			zclose(prog->instances.fds[i]);
	} else if (prog->instances.nr != -1) {
		pr_warning("Internal error: instances.nr is %d\n",
			   prog->instances.nr);
	}

	prog->instances.nr = -1;
	zfree(&prog->instances.fds);
}

static void bpf_program__exit(struct bpf_program *prog)
{
	if (!prog)
		return;

	if (prog->clear_priv)
		prog->clear_priv(prog, prog->priv);

	prog->priv = NULL;
	prog->clear_priv = NULL;

	bpf_program__unload(prog);
	zfree(&prog->name);
	zfree(&prog->section_name);
	zfree(&prog->insns);
	zfree(&prog->reloc_desc);

	prog->nr_reloc = 0;
	prog->insns_cnt = 0;
	prog->idx = -1;
}

static int
bpf_program__init(void *data, size_t size, char *section_name, int idx,
		  struct bpf_program *prog)
{
	if (size < sizeof(struct bpf_insn)) {
		pr_warning("corrupted section '%s'\n", section_name);
		return -EINVAL;
	}

	bzero(prog, sizeof(*prog));

	prog->section_name = strdup(section_name);
	if (!prog->section_name) {
		pr_warning("failed to alloc name for prog under section(%d) %s\n",
			   idx, section_name);
		goto errout;
	}

	prog->insns = malloc(size);
	if (!prog->insns) {
		pr_warning("failed to alloc insns for prog under section %s\n",
			   section_name);
		goto errout;
	}
	prog->insns_cnt = size / sizeof(struct bpf_insn);
	memcpy(prog->insns, data,
	       prog->insns_cnt * sizeof(struct bpf_insn));
	prog->idx = idx;
	prog->instances.fds = NULL;
	prog->instances.nr = -1;
	prog->type = BPF_PROG_TYPE_KPROBE;

	return 0;
errout:
	bpf_program__exit(prog);
	return -ENOMEM;
}

static int
bpf_object__add_program(struct bpf_object *obj, void *data, size_t size,
			char *section_name, int idx)
{
	struct bpf_program prog, *progs;
	int nr_progs, err;

	err = bpf_program__init(data, size, section_name, idx, &prog);
	if (err)
		return err;

	progs = obj->programs;
	nr_progs = obj->nr_programs;

	progs = reallocarray(progs, nr_progs + 1, sizeof(progs[0]));
	if (!progs) {
		/*
		 * In this case the original obj->programs
		 * is still valid, so don't need special treat for
		 * bpf_close_object().
		 */
		pr_warning("failed to alloc a new program under section '%s'\n",
			   section_name);
		bpf_program__exit(&prog);
		return -ENOMEM;
	}

	pr_debug("found program %s\n", prog.section_name);
	obj->programs = progs;
	obj->nr_programs = nr_progs + 1;
	prog.obj = obj;
	progs[nr_progs] = prog;
	return 0;
}

static int
bpf_object__init_prog_names(struct bpf_object *obj)
{
	Elf_Data *symbols = obj->efile.symbols;
	struct bpf_program *prog;
	size_t pi, si;

	for (pi = 0; pi < obj->nr_programs; pi++) {
		const char *name = NULL;

		prog = &obj->programs[pi];

		for (si = 0; si < symbols->d_size / sizeof(GElf_Sym) && !name;
		     si++) {
			GElf_Sym sym;

			if (!gelf_getsym(symbols, si, &sym))
				continue;
			if (sym.st_shndx != prog->idx)
				continue;
			if (GELF_ST_BIND(sym.st_info) != STB_GLOBAL)
				continue;

			name = elf_strptr(obj->efile.elf,
					  obj->efile.strtabidx,
					  sym.st_name);
			if (!name) {
				pr_warning("failed to get sym name string for prog %s\n",
					   prog->section_name);
				return -LIBBPF_ERRNO__LIBELF;
			}
		}

		if (!name && prog->idx == obj->efile.text_shndx)
			name = ".text";

		if (!name) {
			pr_warning("failed to find sym for prog %s\n",
				   prog->section_name);
			return -EINVAL;
		}

		prog->name = strdup(name);
		if (!prog->name) {
			pr_warning("failed to allocate memory for prog sym %s\n",
				   name);
			return -ENOMEM;
		}
	}

	return 0;
}

static struct bpf_object *bpf_object__new(const char *path,
					  void *obj_buf,
					  size_t obj_buf_sz)
{
	struct bpf_object *obj;

	obj = calloc(1, sizeof(struct bpf_object) + strlen(path) + 1);
	if (!obj) {
		pr_warning("alloc memory failed for %s\n", path);
		return ERR_PTR(-ENOMEM);
	}

	strcpy(obj->path, path);
	obj->efile.fd = -1;

	/*
	 * Caller of this function should also calls
	 * bpf_object__elf_finish() after data collection to return
	 * obj_buf to user. If not, we should duplicate the buffer to
	 * avoid user freeing them before elf finish.
	 */
	obj->efile.obj_buf = obj_buf;
	obj->efile.obj_buf_sz = obj_buf_sz;
	obj->efile.maps_shndx = -1;

	obj->loaded = false;

	INIT_LIST_HEAD(&obj->list);
	list_add(&obj->list, &bpf_objects_list);
	return obj;
}

static void bpf_object__elf_finish(struct bpf_object *obj)
{
	if (!obj_elf_valid(obj))
		return;

	if (obj->efile.elf) {
		elf_end(obj->efile.elf);
		obj->efile.elf = NULL;
	}
	obj->efile.symbols = NULL;

	zfree(&obj->efile.reloc);
	obj->efile.nr_reloc = 0;
	zclose(obj->efile.fd);
	obj->efile.obj_buf = NULL;
	obj->efile.obj_buf_sz = 0;
}

static int bpf_object__elf_init(struct bpf_object *obj)
{
	int err = 0;
	GElf_Ehdr *ep;

	if (obj_elf_valid(obj)) {
		pr_warning("elf init: internal error\n");
		return -LIBBPF_ERRNO__LIBELF;
	}

	if (obj->efile.obj_buf_sz > 0) {
		/*
		 * obj_buf should have been validated by
		 * bpf_object__open_buffer().
		 */
		obj->efile.elf = elf_memory(obj->efile.obj_buf,
					    obj->efile.obj_buf_sz);
	} else {
		obj->efile.fd = open(obj->path, O_RDONLY);
		if (obj->efile.fd < 0) {
			char errmsg[STRERR_BUFSIZE];
			char *cp = str_error(errno, errmsg, sizeof(errmsg));

			pr_warning("failed to open %s: %s\n", obj->path, cp);
			return -errno;
		}

		obj->efile.elf = elf_begin(obj->efile.fd,
				LIBBPF_ELF_C_READ_MMAP,
				NULL);
	}

	if (!obj->efile.elf) {
		pr_warning("failed to open %s as ELF file\n",
				obj->path);
		err = -LIBBPF_ERRNO__LIBELF;
		goto errout;
	}

	if (!gelf_getehdr(obj->efile.elf, &obj->efile.ehdr)) {
		pr_warning("failed to get EHDR from %s\n",
				obj->path);
		err = -LIBBPF_ERRNO__FORMAT;
		goto errout;
	}
	ep = &obj->efile.ehdr;

	/* Old LLVM set e_machine to EM_NONE */
	if ((ep->e_type != ET_REL) || (ep->e_machine && (ep->e_machine != EM_BPF))) {
		pr_warning("%s is not an eBPF object file\n",
			obj->path);
		err = -LIBBPF_ERRNO__FORMAT;
		goto errout;
	}

	return 0;
errout:
	bpf_object__elf_finish(obj);
	return err;
}

static int
bpf_object__check_endianness(struct bpf_object *obj)
{
	static unsigned int const endian = 1;

	switch (obj->efile.ehdr.e_ident[EI_DATA]) {
	case ELFDATA2LSB:
		/* We are big endian, BPF obj is little endian. */
		if (*(unsigned char const *)&endian != 1)
			goto mismatch;
		break;

	case ELFDATA2MSB:
		/* We are little endian, BPF obj is big endian. */
		if (*(unsigned char const *)&endian != 0)
			goto mismatch;
		break;
	default:
		return -LIBBPF_ERRNO__ENDIAN;
	}

	return 0;

mismatch:
	pr_warning("Error: endianness mismatch.\n");
	return -LIBBPF_ERRNO__ENDIAN;
}

static int
bpf_object__init_license(struct bpf_object *obj,
			 void *data, size_t size)
{
	memcpy(obj->license, data,
	       min(size, sizeof(obj->license) - 1));
	pr_debug("license of %s is %s\n", obj->path, obj->license);
	return 0;
}

static int
bpf_object__init_kversion(struct bpf_object *obj,
			  void *data, size_t size)
{
	u32 kver;

	if (size != sizeof(kver)) {
		pr_warning("invalid kver section in %s\n", obj->path);
		return -LIBBPF_ERRNO__FORMAT;
	}
	memcpy(&kver, data, sizeof(kver));
	obj->kern_version = kver;
	pr_debug("kernel version of %s is %x\n", obj->path,
		 obj->kern_version);
	return 0;
}

static int compare_bpf_map(const void *_a, const void *_b)
{
	const struct bpf_map *a = _a;
	const struct bpf_map *b = _b;

	return a->offset - b->offset;
}

static int
bpf_object__init_maps(struct bpf_object *obj)
{
	int i, map_idx, map_def_sz, nr_maps = 0;
	Elf_Scn *scn;
	Elf_Data *data;
	Elf_Data *symbols = obj->efile.symbols;

	if (obj->efile.maps_shndx < 0)
		return -EINVAL;
	if (!symbols)
		return -EINVAL;

	scn = elf_getscn(obj->efile.elf, obj->efile.maps_shndx);
	if (scn)
		data = elf_getdata(scn, NULL);
	if (!scn || !data) {
		pr_warning("failed to get Elf_Data from map section %d\n",
			   obj->efile.maps_shndx);
		return -EINVAL;
	}

	/*
	 * Count number of maps. Each map has a name.
	 * Array of maps is not supported: only the first element is
	 * considered.
	 *
	 * TODO: Detect array of map and report error.
	 */
	for (i = 0; i < symbols->d_size / sizeof(GElf_Sym); i++) {
		GElf_Sym sym;

		if (!gelf_getsym(symbols, i, &sym))
			continue;
		if (sym.st_shndx != obj->efile.maps_shndx)
			continue;
		nr_maps++;
	}

	/* Alloc obj->maps and fill nr_maps. */
	pr_debug("maps in %s: %d maps in %zd bytes\n", obj->path,
		 nr_maps, data->d_size);

	if (!nr_maps)
		return 0;

	/* Assume equally sized map definitions */
	map_def_sz = data->d_size / nr_maps;
	if (!data->d_size || (data->d_size % nr_maps) != 0) {
		pr_warning("unable to determine map definition size "
			   "section %s, %d maps in %zd bytes\n",
			   obj->path, nr_maps, data->d_size);
		return -EINVAL;
	}

	obj->maps = calloc(nr_maps, sizeof(obj->maps[0]));
	if (!obj->maps) {
		pr_warning("alloc maps for object failed\n");
		return -ENOMEM;
	}
	obj->nr_maps = nr_maps;

	/*
	 * fill all fd with -1 so won't close incorrect
	 * fd (fd=0 is stdin) when failure (zclose won't close
	 * negative fd)).
	 */
	for (i = 0; i < nr_maps; i++)
		obj->maps[i].fd = -1;

	/*
	 * Fill obj->maps using data in "maps" section.
	 */
	for (i = 0, map_idx = 0; i < symbols->d_size / sizeof(GElf_Sym); i++) {
		GElf_Sym sym;
		const char *map_name;
		struct bpf_map_def *def;

		if (!gelf_getsym(symbols, i, &sym))
			continue;
		if (sym.st_shndx != obj->efile.maps_shndx)
			continue;

		map_name = elf_strptr(obj->efile.elf,
				      obj->efile.strtabidx,
				      sym.st_name);
		obj->maps[map_idx].offset = sym.st_value;
		if (sym.st_value + map_def_sz > data->d_size) {
			pr_warning("corrupted maps section in %s: last map \"%s\" too small\n",
				   obj->path, map_name);
			return -EINVAL;
		}

		obj->maps[map_idx].name = strdup(map_name);
		if (!obj->maps[map_idx].name) {
			pr_warning("failed to alloc map name\n");
			return -ENOMEM;
		}
		pr_debug("map %d is \"%s\"\n", map_idx,
			 obj->maps[map_idx].name);
		def = (struct bpf_map_def *)(data->d_buf + sym.st_value);
		/*
		 * If the definition of the map in the object file fits in
		 * bpf_map_def, copy it.  Any extra fields in our version
		 * of bpf_map_def will default to zero as a result of the
		 * calloc above.
		 */
		if (map_def_sz <= sizeof(struct bpf_map_def)) {
			memcpy(&obj->maps[map_idx].def, def, map_def_sz);
		} else {
			/*
			 * Here the map structure being read is bigger than what
			 * we expect, truncate if the excess bits are all zero.
			 * If they are not zero, reject this map as
			 * incompatible.
			 */
			char *b;
			for (b = ((char *)def) + sizeof(struct bpf_map_def);
			     b < ((char *)def) + map_def_sz; b++) {
				if (*b != 0) {
					pr_warning("maps section in %s: \"%s\" "
						   "has unrecognized, non-zero "
						   "options\n",
						   obj->path, map_name);
					return -EINVAL;
				}
			}
			memcpy(&obj->maps[map_idx].def, def,
			       sizeof(struct bpf_map_def));
		}
		map_idx++;
	}

	qsort(obj->maps, obj->nr_maps, sizeof(obj->maps[0]), compare_bpf_map);
	return 0;
}

static bool section_have_execinstr(struct bpf_object *obj, int idx)
{
	Elf_Scn *scn;
	GElf_Shdr sh;

	scn = elf_getscn(obj->efile.elf, idx);
	if (!scn)
		return false;

	if (gelf_getshdr(scn, &sh) != &sh)
		return false;

	if (sh.sh_flags & SHF_EXECINSTR)
		return true;

	return false;
}

static int bpf_object__elf_collect(struct bpf_object *obj)
{
	Elf *elf = obj->efile.elf;
	GElf_Ehdr *ep = &obj->efile.ehdr;
	Elf_Scn *scn = NULL;
	int idx = 0, err = 0;

	/* Elf is corrupted/truncated, avoid calling elf_strptr. */
	if (!elf_rawdata(elf_getscn(elf, ep->e_shstrndx), NULL)) {
		pr_warning("failed to get e_shstrndx from %s\n",
			   obj->path);
		return -LIBBPF_ERRNO__FORMAT;
	}

	while ((scn = elf_nextscn(elf, scn)) != NULL) {
		char *name;
		GElf_Shdr sh;
		Elf_Data *data;

		idx++;
		if (gelf_getshdr(scn, &sh) != &sh) {
			pr_warning("failed to get section(%d) header from %s\n",
				   idx, obj->path);
			err = -LIBBPF_ERRNO__FORMAT;
			goto out;
		}

		name = elf_strptr(elf, ep->e_shstrndx, sh.sh_name);
		if (!name) {
			pr_warning("failed to get section(%d) name from %s\n",
				   idx, obj->path);
			err = -LIBBPF_ERRNO__FORMAT;
			goto out;
		}

		data = elf_getdata(scn, 0);
		if (!data) {
			pr_warning("failed to get section(%d) data from %s(%s)\n",
				   idx, name, obj->path);
			err = -LIBBPF_ERRNO__FORMAT;
			goto out;
		}
		pr_debug("section(%d) %s, size %ld, link %d, flags %lx, type=%d\n",
			 idx, name, (unsigned long)data->d_size,
			 (int)sh.sh_link, (unsigned long)sh.sh_flags,
			 (int)sh.sh_type);

		if (strcmp(name, "license") == 0)
			err = bpf_object__init_license(obj,
						       data->d_buf,
						       data->d_size);
		else if (strcmp(name, "version") == 0)
			err = bpf_object__init_kversion(obj,
							data->d_buf,
							data->d_size);
		else if (strcmp(name, "maps") == 0)
			obj->efile.maps_shndx = idx;
		else if (strcmp(name, BTF_ELF_SEC) == 0) {
			obj->btf = btf__new(data->d_buf, data->d_size,
					    __pr_debug);
			if (IS_ERR(obj->btf)) {
				pr_warning("Error loading ELF section %s: %ld. Ignored and continue.\n",
					   BTF_ELF_SEC, PTR_ERR(obj->btf));
				obj->btf = NULL;
			}
		} else if (sh.sh_type == SHT_SYMTAB) {
			if (obj->efile.symbols) {
				pr_warning("bpf: multiple SYMTAB in %s\n",
					   obj->path);
				err = -LIBBPF_ERRNO__FORMAT;
			} else {
				obj->efile.symbols = data;
				obj->efile.strtabidx = sh.sh_link;
			}
		} else if ((sh.sh_type == SHT_PROGBITS) &&
			   (sh.sh_flags & SHF_EXECINSTR) &&
			   (data->d_size > 0)) {
			if (strcmp(name, ".text") == 0)
				obj->efile.text_shndx = idx;
			err = bpf_object__add_program(obj, data->d_buf,
						      data->d_size, name, idx);
			if (err) {
				char errmsg[STRERR_BUFSIZE];
				char *cp = str_error(-err, errmsg, sizeof(errmsg));

				pr_warning("failed to alloc program %s (%s): %s",
					   name, obj->path, cp);
			}
		} else if (sh.sh_type == SHT_REL) {
			void *reloc = obj->efile.reloc;
			int nr_reloc = obj->efile.nr_reloc + 1;
			int sec = sh.sh_info; /* points to other section */

			/* Only do relo for section with exec instructions */
			if (!section_have_execinstr(obj, sec)) {
				pr_debug("skip relo %s(%d) for section(%d)\n",
					 name, idx, sec);
				continue;
			}

			reloc = reallocarray(reloc, nr_reloc,
					     sizeof(*obj->efile.reloc));
			if (!reloc) {
				pr_warning("realloc failed\n");
				err = -ENOMEM;
			} else {
				int n = nr_reloc - 1;

				obj->efile.reloc = reloc;
				obj->efile.nr_reloc = nr_reloc;

				obj->efile.reloc[n].shdr = sh;
				obj->efile.reloc[n].data = data;
			}
		} else {
			pr_debug("skip section(%d) %s\n", idx, name);
		}
		if (err)
			goto out;
	}

	if (!obj->efile.strtabidx || obj->efile.strtabidx >= idx) {
		pr_warning("Corrupted ELF file: index of strtab invalid\n");
		return LIBBPF_ERRNO__FORMAT;
	}
	if (obj->efile.maps_shndx >= 0) {
		err = bpf_object__init_maps(obj);
		if (err)
			goto out;
	}
	err = bpf_object__init_prog_names(obj);
out:
	return err;
}

static struct bpf_program *
bpf_object__find_prog_by_idx(struct bpf_object *obj, int idx)
{
	struct bpf_program *prog;
	size_t i;

	for (i = 0; i < obj->nr_programs; i++) {
		prog = &obj->programs[i];
		if (prog->idx == idx)
			return prog;
	}
	return NULL;
}

struct bpf_program *
bpf_object__find_program_by_title(struct bpf_object *obj, const char *title)
{
	struct bpf_program *pos;

	bpf_object__for_each_program(pos, obj) {
		if (pos->section_name && !strcmp(pos->section_name, title))
			return pos;
	}
	return NULL;
}

static int
bpf_program__collect_reloc(struct bpf_program *prog, GElf_Shdr *shdr,
			   Elf_Data *data, struct bpf_object *obj)
{
	Elf_Data *symbols = obj->efile.symbols;
	int text_shndx = obj->efile.text_shndx;
	int maps_shndx = obj->efile.maps_shndx;
	struct bpf_map *maps = obj->maps;
	size_t nr_maps = obj->nr_maps;
	int i, nrels;

	pr_debug("collecting relocating info for: '%s'\n",
		 prog->section_name);
	nrels = shdr->sh_size / shdr->sh_entsize;

	prog->reloc_desc = malloc(sizeof(*prog->reloc_desc) * nrels);
	if (!prog->reloc_desc) {
		pr_warning("failed to alloc memory in relocation\n");
		return -ENOMEM;
	}
	prog->nr_reloc = nrels;

	for (i = 0; i < nrels; i++) {
		GElf_Sym sym;
		GElf_Rel rel;
		unsigned int insn_idx;
		struct bpf_insn *insns = prog->insns;
		size_t map_idx;

		if (!gelf_getrel(data, i, &rel)) {
			pr_warning("relocation: failed to get %d reloc\n", i);
			return -LIBBPF_ERRNO__FORMAT;
		}

		if (!gelf_getsym(symbols,
				 GELF_R_SYM(rel.r_info),
				 &sym)) {
			pr_warning("relocation: symbol %"PRIx64" not found\n",
				   GELF_R_SYM(rel.r_info));
			return -LIBBPF_ERRNO__FORMAT;
		}
		pr_debug("relo for %lld value %lld name %d\n",
			 (long long) (rel.r_info >> 32),
			 (long long) sym.st_value, sym.st_name);

		if (sym.st_shndx != maps_shndx && sym.st_shndx != text_shndx) {
			pr_warning("Program '%s' contains non-map related relo data pointing to section %u\n",
				   prog->section_name, sym.st_shndx);
			return -LIBBPF_ERRNO__RELOC;
		}

		insn_idx = rel.r_offset / sizeof(struct bpf_insn);
		pr_debug("relocation: insn_idx=%u\n", insn_idx);

		if (insns[insn_idx].code == (BPF_JMP | BPF_CALL)) {
			if (insns[insn_idx].src_reg != BPF_PSEUDO_CALL) {
				pr_warning("incorrect bpf_call opcode\n");
				return -LIBBPF_ERRNO__RELOC;
			}
			prog->reloc_desc[i].type = RELO_CALL;
			prog->reloc_desc[i].insn_idx = insn_idx;
			prog->reloc_desc[i].text_off = sym.st_value;
			obj->has_pseudo_calls = true;
			continue;
		}

		if (insns[insn_idx].code != (BPF_LD | BPF_IMM | BPF_DW)) {
			pr_warning("bpf: relocation: invalid relo for insns[%d].code 0x%x\n",
				   insn_idx, insns[insn_idx].code);
			return -LIBBPF_ERRNO__RELOC;
		}

		/* TODO: 'maps' is sorted. We can use bsearch to make it faster. */
		for (map_idx = 0; map_idx < nr_maps; map_idx++) {
			if (maps[map_idx].offset == sym.st_value) {
				pr_debug("relocation: find map %zd (%s) for insn %u\n",
					 map_idx, maps[map_idx].name, insn_idx);
				break;
			}
		}

		if (map_idx >= nr_maps) {
			pr_warning("bpf relocation: map_idx %d large than %d\n",
				   (int)map_idx, (int)nr_maps - 1);
			return -LIBBPF_ERRNO__RELOC;
		}

		prog->reloc_desc[i].type = RELO_LD64;
		prog->reloc_desc[i].insn_idx = insn_idx;
		prog->reloc_desc[i].map_idx = map_idx;
	}
	return 0;
}

static int bpf_map_find_btf_info(struct bpf_map *map, const struct btf *btf)
{
	const struct btf_type *container_type;
	const struct btf_member *key, *value;
	struct bpf_map_def *def = &map->def;
	const size_t max_name = 256;
	char container_name[max_name];
	__s64 key_size, value_size;
	__s32 container_id;

	if (snprintf(container_name, max_name, "____btf_map_%s", map->name) ==
	    max_name) {
		pr_warning("map:%s length of '____btf_map_%s' is too long\n",
			   map->name, map->name);
		return -EINVAL;
	}

	container_id = btf__find_by_name(btf, container_name);
	if (container_id < 0) {
		pr_debug("map:%s container_name:%s cannot be found in BTF. Missing BPF_ANNOTATE_KV_PAIR?\n",
			 map->name, container_name);
		return container_id;
	}

	container_type = btf__type_by_id(btf, container_id);
	if (!container_type) {
		pr_warning("map:%s cannot find BTF type for container_id:%u\n",
			   map->name, container_id);
		return -EINVAL;
	}

	if (BTF_INFO_KIND(container_type->info) != BTF_KIND_STRUCT ||
	    BTF_INFO_VLEN(container_type->info) < 2) {
		pr_warning("map:%s container_name:%s is an invalid container struct\n",
			   map->name, container_name);
		return -EINVAL;
	}

	key = (struct btf_member *)(container_type + 1);
	value = key + 1;

	key_size = btf__resolve_size(btf, key->type);
	if (key_size < 0) {
		pr_warning("map:%s invalid BTF key_type_size\n",
			   map->name);
		return key_size;
	}

	if (def->key_size != key_size) {
		pr_warning("map:%s btf_key_type_size:%u != map_def_key_size:%u\n",
			   map->name, (__u32)key_size, def->key_size);
		return -EINVAL;
	}

	value_size = btf__resolve_size(btf, value->type);
	if (value_size < 0) {
		pr_warning("map:%s invalid BTF value_type_size\n", map->name);
		return value_size;
	}

	if (def->value_size != value_size) {
		pr_warning("map:%s btf_value_type_size:%u != map_def_value_size:%u\n",
			   map->name, (__u32)value_size, def->value_size);
		return -EINVAL;
	}

	map->btf_key_type_id = key->type;
	map->btf_value_type_id = value->type;

	return 0;
}

int bpf_map__reuse_fd(struct bpf_map *map, int fd)
{
	struct bpf_map_info info = {};
	__u32 len = sizeof(info);
	int new_fd, err;
	char *new_name;

	err = bpf_obj_get_info_by_fd(fd, &info, &len);
	if (err)
		return err;

	new_name = strdup(info.name);
	if (!new_name)
		return -errno;

	new_fd = open("/", O_RDONLY | O_CLOEXEC);
	if (new_fd < 0) {
		err = -errno;
		goto err_free_new_name;
	}

	new_fd = dup3(fd, new_fd, O_CLOEXEC);
	if (new_fd < 0) {
		err = -errno;
		goto err_close_new_fd;
	}

	err = zclose(map->fd);
	if (err) {
		err = -errno;
		goto err_close_new_fd;
	}
	free(map->name);

	map->fd = new_fd;
	map->name = new_name;
	map->def.type = info.type;
	map->def.key_size = info.key_size;
	map->def.value_size = info.value_size;
	map->def.max_entries = info.max_entries;
	map->def.map_flags = info.map_flags;
	map->btf_key_type_id = info.btf_key_type_id;
	map->btf_value_type_id = info.btf_value_type_id;

	return 0;

err_close_new_fd:
	close(new_fd);
err_free_new_name:
	free(new_name);
	return err;
}

static int
bpf_object__create_maps(struct bpf_object *obj)
{
	struct bpf_create_map_attr create_attr = {};
	unsigned int i;
	int err;

	for (i = 0; i < obj->nr_maps; i++) {
		struct bpf_map *map = &obj->maps[i];
		struct bpf_map_def *def = &map->def;
		char *cp, errmsg[STRERR_BUFSIZE];
		int *pfd = &map->fd;

		if (map->fd >= 0) {
			pr_debug("skip map create (preset) %s: fd=%d\n",
				 map->name, map->fd);
			continue;
		}

		create_attr.name = map->name;
		create_attr.map_ifindex = map->map_ifindex;
		create_attr.map_type = def->type;
		create_attr.map_flags = def->map_flags;
		create_attr.key_size = def->key_size;
		create_attr.value_size = def->value_size;
		create_attr.max_entries = def->max_entries;
		create_attr.btf_fd = 0;
		create_attr.btf_key_type_id = 0;
		create_attr.btf_value_type_id = 0;

		if (obj->btf && !bpf_map_find_btf_info(map, obj->btf)) {
			create_attr.btf_fd = btf__fd(obj->btf);
			create_attr.btf_key_type_id = map->btf_key_type_id;
			create_attr.btf_value_type_id = map->btf_value_type_id;
		}

		*pfd = bpf_create_map_xattr(&create_attr);
		if (*pfd < 0 && create_attr.btf_key_type_id) {
			cp = str_error(errno, errmsg, sizeof(errmsg));
			pr_warning("Error in bpf_create_map_xattr(%s):%s(%d). Retrying without BTF.\n",
				   map->name, cp, errno);
			create_attr.btf_fd = 0;
			create_attr.btf_key_type_id = 0;
			create_attr.btf_value_type_id = 0;
			map->btf_key_type_id = 0;
			map->btf_value_type_id = 0;
			*pfd = bpf_create_map_xattr(&create_attr);
		}

		if (*pfd < 0) {
			size_t j;

			err = *pfd;
			cp = str_error(errno, errmsg, sizeof(errmsg));
			pr_warning("failed to create map (name: '%s'): %s\n",
				   map->name, cp);
			for (j = 0; j < i; j++)
				zclose(obj->maps[j].fd);
			return err;
		}
		pr_debug("create map %s: fd=%d\n", map->name, *pfd);
	}

	return 0;
}

static int
bpf_program__reloc_text(struct bpf_program *prog, struct bpf_object *obj,
			struct reloc_desc *relo)
{
	struct bpf_insn *insn, *new_insn;
	struct bpf_program *text;
	size_t new_cnt;

	if (relo->type != RELO_CALL)
		return -LIBBPF_ERRNO__RELOC;

	if (prog->idx == obj->efile.text_shndx) {
		pr_warning("relo in .text insn %d into off %d\n",
			   relo->insn_idx, relo->text_off);
		return -LIBBPF_ERRNO__RELOC;
	}

	if (prog->main_prog_cnt == 0) {
		text = bpf_object__find_prog_by_idx(obj, obj->efile.text_shndx);
		if (!text) {
			pr_warning("no .text section found yet relo into text exist\n");
			return -LIBBPF_ERRNO__RELOC;
		}
		new_cnt = prog->insns_cnt + text->insns_cnt;
		new_insn = reallocarray(prog->insns, new_cnt, sizeof(*insn));
		if (!new_insn) {
			pr_warning("oom in prog realloc\n");
			return -ENOMEM;
		}
		memcpy(new_insn + prog->insns_cnt, text->insns,
		       text->insns_cnt * sizeof(*insn));
		prog->insns = new_insn;
		prog->main_prog_cnt = prog->insns_cnt;
		prog->insns_cnt = new_cnt;
		pr_debug("added %zd insn from %s to prog %s\n",
			 text->insns_cnt, text->section_name,
			 prog->section_name);
	}
	insn = &prog->insns[relo->insn_idx];
	insn->imm += prog->main_prog_cnt - relo->insn_idx;
	return 0;
}

static int
bpf_program__relocate(struct bpf_program *prog, struct bpf_object *obj)
{
	int i, err;

	if (!prog || !prog->reloc_desc)
		return 0;

	for (i = 0; i < prog->nr_reloc; i++) {
		if (prog->reloc_desc[i].type == RELO_LD64) {
			struct bpf_insn *insns = prog->insns;
			int insn_idx, map_idx;

			insn_idx = prog->reloc_desc[i].insn_idx;
			map_idx = prog->reloc_desc[i].map_idx;

			if (insn_idx >= (int)prog->insns_cnt) {
				pr_warning("relocation out of range: '%s'\n",
					   prog->section_name);
				return -LIBBPF_ERRNO__RELOC;
			}
			insns[insn_idx].src_reg = BPF_PSEUDO_MAP_FD;
			insns[insn_idx].imm = obj->maps[map_idx].fd;
		} else {
			err = bpf_program__reloc_text(prog, obj,
						      &prog->reloc_desc[i]);
			if (err)
				return err;
		}
	}

	zfree(&prog->reloc_desc);
	prog->nr_reloc = 0;
	return 0;
}


static int
bpf_object__relocate(struct bpf_object *obj)
{
	struct bpf_program *prog;
	size_t i;
	int err;

	for (i = 0; i < obj->nr_programs; i++) {
		prog = &obj->programs[i];

		err = bpf_program__relocate(prog, obj);
		if (err) {
			pr_warning("failed to relocate '%s'\n",
				   prog->section_name);
			return err;
		}
	}
	return 0;
}

static int bpf_object__collect_reloc(struct bpf_object *obj)
{
	int i, err;

	if (!obj_elf_valid(obj)) {
		pr_warning("Internal error: elf object is closed\n");
		return -LIBBPF_ERRNO__INTERNAL;
	}

	for (i = 0; i < obj->efile.nr_reloc; i++) {
		GElf_Shdr *shdr = &obj->efile.reloc[i].shdr;
		Elf_Data *data = obj->efile.reloc[i].data;
		int idx = shdr->sh_info;
		struct bpf_program *prog;

		if (shdr->sh_type != SHT_REL) {
			pr_warning("internal error at %d\n", __LINE__);
			return -LIBBPF_ERRNO__INTERNAL;
		}

		prog = bpf_object__find_prog_by_idx(obj, idx);
		if (!prog) {
			pr_warning("relocation failed: no section(%d)\n", idx);
			return -LIBBPF_ERRNO__RELOC;
		}

		err = bpf_program__collect_reloc(prog,
						 shdr, data,
						 obj);
		if (err)
			return err;
	}
	return 0;
}

static int
load_program(enum bpf_prog_type type, enum bpf_attach_type expected_attach_type,
	     const char *name, struct bpf_insn *insns, int insns_cnt,
	     char *license, u32 kern_version, int *pfd, int prog_ifindex)
{
	struct bpf_load_program_attr load_attr;
	char *cp, errmsg[STRERR_BUFSIZE];
	char *log_buf;
	int ret;

	memset(&load_attr, 0, sizeof(struct bpf_load_program_attr));
	load_attr.prog_type = type;
	load_attr.expected_attach_type = expected_attach_type;
	load_attr.name = name;
	load_attr.insns = insns;
	load_attr.insns_cnt = insns_cnt;
	load_attr.license = license;
	load_attr.kern_version = kern_version;
	load_attr.prog_ifindex = prog_ifindex;

	if (!load_attr.insns || !load_attr.insns_cnt)
		return -EINVAL;

	log_buf = malloc(BPF_LOG_BUF_SIZE);
	if (!log_buf)
		pr_warning("Alloc log buffer for bpf loader error, continue without log\n");

	ret = bpf_load_program_xattr(&load_attr, log_buf, BPF_LOG_BUF_SIZE);

	if (ret >= 0) {
		*pfd = ret;
		ret = 0;
		goto out;
	}

	ret = -LIBBPF_ERRNO__LOAD;
	cp = str_error(errno, errmsg, sizeof(errmsg));
	pr_warning("load bpf program failed: %s\n", cp);

	if (log_buf && log_buf[0] != '\0') {
		ret = -LIBBPF_ERRNO__VERIFY;
		pr_warning("-- BEGIN DUMP LOG ---\n");
		pr_warning("\n%s\n", log_buf);
		pr_warning("-- END LOG --\n");
	} else if (load_attr.insns_cnt >= BPF_MAXINSNS) {
		pr_warning("Program too large (%zu insns), at most %d insns\n",
			   load_attr.insns_cnt, BPF_MAXINSNS);
		ret = -LIBBPF_ERRNO__PROG2BIG;
	} else {
		/* Wrong program type? */
		if (load_attr.prog_type != BPF_PROG_TYPE_KPROBE) {
			int fd;

			load_attr.prog_type = BPF_PROG_TYPE_KPROBE;
			load_attr.expected_attach_type = 0;
			fd = bpf_load_program_xattr(&load_attr, NULL, 0);
			if (fd >= 0) {
				close(fd);
				ret = -LIBBPF_ERRNO__PROGTYPE;
				goto out;
			}
		}

		if (log_buf)
			ret = -LIBBPF_ERRNO__KVER;
	}

out:
	free(log_buf);
	return ret;
}

static int
bpf_program__load(struct bpf_program *prog,
		  char *license, u32 kern_version)
{
	int err = 0, fd, i;

	if (prog->instances.nr < 0 || !prog->instances.fds) {
		if (prog->preprocessor) {
			pr_warning("Internal error: can't load program '%s'\n",
				   prog->section_name);
			return -LIBBPF_ERRNO__INTERNAL;
		}

		prog->instances.fds = malloc(sizeof(int));
		if (!prog->instances.fds) {
			pr_warning("Not enough memory for BPF fds\n");
			return -ENOMEM;
		}
		prog->instances.nr = 1;
		prog->instances.fds[0] = -1;
	}

	if (!prog->preprocessor) {
		if (prog->instances.nr != 1) {
			pr_warning("Program '%s' is inconsistent: nr(%d) != 1\n",
				   prog->section_name, prog->instances.nr);
		}
		err = load_program(prog->type, prog->expected_attach_type,
				   prog->name, prog->insns, prog->insns_cnt,
				   license, kern_version, &fd,
				   prog->prog_ifindex);
		if (!err)
			prog->instances.fds[0] = fd;
		goto out;
	}

	for (i = 0; i < prog->instances.nr; i++) {
		struct bpf_prog_prep_result result;
		bpf_program_prep_t preprocessor = prog->preprocessor;

		bzero(&result, sizeof(result));
		err = preprocessor(prog, i, prog->insns,
				   prog->insns_cnt, &result);
		if (err) {
			pr_warning("Preprocessing the %dth instance of program '%s' failed\n",
				   i, prog->section_name);
			goto out;
		}

		if (!result.new_insn_ptr || !result.new_insn_cnt) {
			pr_debug("Skip loading the %dth instance of program '%s'\n",
				 i, prog->section_name);
			prog->instances.fds[i] = -1;
			if (result.pfd)
				*result.pfd = -1;
			continue;
		}

		err = load_program(prog->type, prog->expected_attach_type,
				   prog->name, result.new_insn_ptr,
				   result.new_insn_cnt,
				   license, kern_version, &fd,
				   prog->prog_ifindex);

		if (err) {
			pr_warning("Loading the %dth instance of program '%s' failed\n",
					i, prog->section_name);
			goto out;
		}

		if (result.pfd)
			*result.pfd = fd;
		prog->instances.fds[i] = fd;
	}
out:
	if (err)
		pr_warning("failed to load program '%s'\n",
			   prog->section_name);
	zfree(&prog->insns);
	prog->insns_cnt = 0;
	return err;
}

static bool bpf_program__is_function_storage(struct bpf_program *prog,
					     struct bpf_object *obj)
{
	return prog->idx == obj->efile.text_shndx && obj->has_pseudo_calls;
}

static int
bpf_object__load_progs(struct bpf_object *obj)
{
	size_t i;
	int err;

	for (i = 0; i < obj->nr_programs; i++) {
		if (bpf_program__is_function_storage(&obj->programs[i], obj))
			continue;
		err = bpf_program__load(&obj->programs[i],
					obj->license,
					obj->kern_version);
		if (err)
			return err;
	}
	return 0;
}

static bool bpf_prog_type__needs_kver(enum bpf_prog_type type)
{
	switch (type) {
	case BPF_PROG_TYPE_SOCKET_FILTER:
	case BPF_PROG_TYPE_SCHED_CLS:
	case BPF_PROG_TYPE_SCHED_ACT:
	case BPF_PROG_TYPE_XDP:
	case BPF_PROG_TYPE_CGROUP_SKB:
	case BPF_PROG_TYPE_CGROUP_SOCK:
	case BPF_PROG_TYPE_LWT_IN:
	case BPF_PROG_TYPE_LWT_OUT:
	case BPF_PROG_TYPE_LWT_XMIT:
	case BPF_PROG_TYPE_LWT_SEG6LOCAL:
	case BPF_PROG_TYPE_SOCK_OPS:
	case BPF_PROG_TYPE_SK_SKB:
	case BPF_PROG_TYPE_CGROUP_DEVICE:
	case BPF_PROG_TYPE_SK_MSG:
	case BPF_PROG_TYPE_CGROUP_SOCK_ADDR:
	case BPF_PROG_TYPE_LIRC_MODE2:
	case BPF_PROG_TYPE_SK_REUSEPORT:
		return false;
	case BPF_PROG_TYPE_UNSPEC:
	case BPF_PROG_TYPE_KPROBE:
	case BPF_PROG_TYPE_TRACEPOINT:
	case BPF_PROG_TYPE_PERF_EVENT:
	case BPF_PROG_TYPE_RAW_TRACEPOINT:
	default:
		return true;
	}
}

static int bpf_object__validate(struct bpf_object *obj, bool needs_kver)
{
	if (needs_kver && obj->kern_version == 0) {
		pr_warning("%s doesn't provide kernel version\n",
			   obj->path);
		return -LIBBPF_ERRNO__KVERSION;
	}
	return 0;
}

static struct bpf_object *
__bpf_object__open(const char *path, void *obj_buf, size_t obj_buf_sz,
		   bool needs_kver)
{
	struct bpf_object *obj;
	int err;

	if (elf_version(EV_CURRENT) == EV_NONE) {
		pr_warning("failed to init libelf for %s\n", path);
		return ERR_PTR(-LIBBPF_ERRNO__LIBELF);
	}

	obj = bpf_object__new(path, obj_buf, obj_buf_sz);
	if (IS_ERR(obj))
		return obj;

	CHECK_ERR(bpf_object__elf_init(obj), err, out);
	CHECK_ERR(bpf_object__check_endianness(obj), err, out);
	CHECK_ERR(bpf_object__elf_collect(obj), err, out);
	CHECK_ERR(bpf_object__collect_reloc(obj), err, out);
	CHECK_ERR(bpf_object__validate(obj, needs_kver), err, out);

	bpf_object__elf_finish(obj);
	return obj;
out:
	bpf_object__close(obj);
	return ERR_PTR(err);
}

struct bpf_object *bpf_object__open_xattr(struct bpf_object_open_attr *attr)
{
	/* param validation */
	if (!attr->file)
		return NULL;

	pr_debug("loading %s\n", attr->file);

	return __bpf_object__open(attr->file, NULL, 0,
				  bpf_prog_type__needs_kver(attr->prog_type));
}

struct bpf_object *bpf_object__open(const char *path)
{
	struct bpf_object_open_attr attr = {
		.file		= path,
		.prog_type	= BPF_PROG_TYPE_UNSPEC,
	};

	return bpf_object__open_xattr(&attr);
}

struct bpf_object *bpf_object__open_buffer(void *obj_buf,
					   size_t obj_buf_sz,
					   const char *name)
{
	char tmp_name[64];

	/* param validation */
	if (!obj_buf || obj_buf_sz <= 0)
		return NULL;

	if (!name) {
		snprintf(tmp_name, sizeof(tmp_name), "%lx-%lx",
			 (unsigned long)obj_buf,
			 (unsigned long)obj_buf_sz);
		tmp_name[sizeof(tmp_name) - 1] = '\0';
		name = tmp_name;
	}
	pr_debug("loading object '%s' from buffer\n",
		 name);

	return __bpf_object__open(name, obj_buf, obj_buf_sz, true);
}

int bpf_object__unload(struct bpf_object *obj)
{
	size_t i;

	if (!obj)
		return -EINVAL;

	for (i = 0; i < obj->nr_maps; i++)
		zclose(obj->maps[i].fd);

	for (i = 0; i < obj->nr_programs; i++)
		bpf_program__unload(&obj->programs[i]);

	return 0;
}

int bpf_object__load(struct bpf_object *obj)
{
	int err;

	if (!obj)
		return -EINVAL;

	if (obj->loaded) {
		pr_warning("object should not be loaded twice\n");
		return -EINVAL;
	}

	obj->loaded = true;

	CHECK_ERR(bpf_object__create_maps(obj), err, out);
	CHECK_ERR(bpf_object__relocate(obj), err, out);
	CHECK_ERR(bpf_object__load_progs(obj), err, out);

	return 0;
out:
	bpf_object__unload(obj);
	pr_warning("failed to load object '%s'\n", obj->path);
	return err;
}

static int check_path(const char *path)
{
	char *cp, errmsg[STRERR_BUFSIZE];
	struct statfs st_fs;
	char *dname, *dir;
	int err = 0;

	if (path == NULL)
		return -EINVAL;

	dname = strdup(path);
	if (dname == NULL)
		return -ENOMEM;

	dir = dirname(dname);
	if (statfs(dir, &st_fs)) {
		cp = str_error(errno, errmsg, sizeof(errmsg));
		pr_warning("failed to statfs %s: %s\n", dir, cp);
		err = -errno;
	}
	free(dname);

	if (!err && st_fs.f_type != BPF_FS_MAGIC) {
		pr_warning("specified path %s is not on BPF FS\n", path);
		err = -EINVAL;
	}

	return err;
}

int bpf_program__pin_instance(struct bpf_program *prog, const char *path,
			      int instance)
{
	char *cp, errmsg[STRERR_BUFSIZE];
	int err;

	err = check_path(path);
	if (err)
		return err;

	if (prog == NULL) {
		pr_warning("invalid program pointer\n");
		return -EINVAL;
	}

	if (instance < 0 || instance >= prog->instances.nr) {
		pr_warning("invalid prog instance %d of prog %s (max %d)\n",
			   instance, prog->section_name, prog->instances.nr);
		return -EINVAL;
	}

	if (bpf_obj_pin(prog->instances.fds[instance], path)) {
		cp = str_error(errno, errmsg, sizeof(errmsg));
		pr_warning("failed to pin program: %s\n", cp);
		return -errno;
	}
	pr_debug("pinned program '%s'\n", path);

	return 0;
}

static int make_dir(const char *path)
{
	char *cp, errmsg[STRERR_BUFSIZE];
	int err = 0;

	if (mkdir(path, 0700) && errno != EEXIST)
		err = -errno;

	if (err) {
		cp = str_error(-err, errmsg, sizeof(errmsg));
		pr_warning("failed to mkdir %s: %s\n", path, cp);
	}
	return err;
}

int bpf_program__pin(struct bpf_program *prog, const char *path)
{
	int i, err;

	err = check_path(path);
	if (err)
		return err;

	if (prog == NULL) {
		pr_warning("invalid program pointer\n");
		return -EINVAL;
	}

	if (prog->instances.nr <= 0) {
		pr_warning("no instances of prog %s to pin\n",
			   prog->section_name);
		return -EINVAL;
	}

	err = make_dir(path);
	if (err)
		return err;

	for (i = 0; i < prog->instances.nr; i++) {
		char buf[PATH_MAX];
		int len;

		len = snprintf(buf, PATH_MAX, "%s/%d", path, i);
		if (len < 0)
			return -EINVAL;
		else if (len >= PATH_MAX)
			return -ENAMETOOLONG;

		err = bpf_program__pin_instance(prog, buf, i);
		if (err)
			return err;
	}

	return 0;
}

int bpf_map__pin(struct bpf_map *map, const char *path)
{
	char *cp, errmsg[STRERR_BUFSIZE];
	int err;

	err = check_path(path);
	if (err)
		return err;

	if (map == NULL) {
		pr_warning("invalid map pointer\n");
		return -EINVAL;
	}

	if (bpf_obj_pin(map->fd, path)) {
		cp = str_error(errno, errmsg, sizeof(errmsg));
		pr_warning("failed to pin map: %s\n", cp);
		return -errno;
	}

	pr_debug("pinned map '%s'\n", path);
	return 0;
}

int bpf_object__pin(struct bpf_object *obj, const char *path)
{
	struct bpf_program *prog;
	struct bpf_map *map;
	int err;

	if (!obj)
		return -ENOENT;

	if (!obj->loaded) {
		pr_warning("object not yet loaded; load it first\n");
		return -ENOENT;
	}

	err = make_dir(path);
	if (err)
		return err;

	bpf_map__for_each(map, obj) {
		char buf[PATH_MAX];
		int len;

		len = snprintf(buf, PATH_MAX, "%s/%s", path,
			       bpf_map__name(map));
		if (len < 0)
			return -EINVAL;
		else if (len >= PATH_MAX)
			return -ENAMETOOLONG;

		err = bpf_map__pin(map, buf);
		if (err)
			return err;
	}

	bpf_object__for_each_program(prog, obj) {
		char buf[PATH_MAX];
		int len;

		len = snprintf(buf, PATH_MAX, "%s/%s", path,
			       prog->section_name);
		if (len < 0)
			return -EINVAL;
		else if (len >= PATH_MAX)
			return -ENAMETOOLONG;

		err = bpf_program__pin(prog, buf);
		if (err)
			return err;
	}

	return 0;
}

void bpf_object__close(struct bpf_object *obj)
{
	size_t i;

	if (!obj)
		return;

	if (obj->clear_priv)
		obj->clear_priv(obj, obj->priv);

	bpf_object__elf_finish(obj);
	bpf_object__unload(obj);
	btf__free(obj->btf);

	for (i = 0; i < obj->nr_maps; i++) {
		zfree(&obj->maps[i].name);
		if (obj->maps[i].clear_priv)
			obj->maps[i].clear_priv(&obj->maps[i],
						obj->maps[i].priv);
		obj->maps[i].priv = NULL;
		obj->maps[i].clear_priv = NULL;
	}
	zfree(&obj->maps);
	obj->nr_maps = 0;

	if (obj->programs && obj->nr_programs) {
		for (i = 0; i < obj->nr_programs; i++)
			bpf_program__exit(&obj->programs[i]);
	}
	zfree(&obj->programs);

	list_del(&obj->list);
	free(obj);
}

struct bpf_object *
bpf_object__next(struct bpf_object *prev)
{
	struct bpf_object *next;

	if (!prev)
		next = list_first_entry(&bpf_objects_list,
					struct bpf_object,
					list);
	else
		next = list_next_entry(prev, list);

	/* Empty list is noticed here so don't need checking on entry. */
	if (&next->list == &bpf_objects_list)
		return NULL;

	return next;
}

const char *bpf_object__name(struct bpf_object *obj)
{
	return obj ? obj->path : ERR_PTR(-EINVAL);
}

unsigned int bpf_object__kversion(struct bpf_object *obj)
{
	return obj ? obj->kern_version : 0;
}

int bpf_object__btf_fd(const struct bpf_object *obj)
{
	return obj->btf ? btf__fd(obj->btf) : -1;
}

int bpf_object__set_priv(struct bpf_object *obj, void *priv,
			 bpf_object_clear_priv_t clear_priv)
{
	if (obj->priv && obj->clear_priv)
		obj->clear_priv(obj, obj->priv);

	obj->priv = priv;
	obj->clear_priv = clear_priv;
	return 0;
}

void *bpf_object__priv(struct bpf_object *obj)
{
	return obj ? obj->priv : ERR_PTR(-EINVAL);
}

static struct bpf_program *
__bpf_program__next(struct bpf_program *prev, struct bpf_object *obj)
{
	size_t idx;

	if (!obj->programs)
		return NULL;
	/* First handler */
	if (prev == NULL)
		return &obj->programs[0];

	if (prev->obj != obj) {
		pr_warning("error: program handler doesn't match object\n");
		return NULL;
	}

	idx = (prev - obj->programs) + 1;
	if (idx >= obj->nr_programs)
		return NULL;
	return &obj->programs[idx];
}

struct bpf_program *
bpf_program__next(struct bpf_program *prev, struct bpf_object *obj)
{
	struct bpf_program *prog = prev;

	do {
		prog = __bpf_program__next(prog, obj);
	} while (prog && bpf_program__is_function_storage(prog, obj));

	return prog;
}

int bpf_program__set_priv(struct bpf_program *prog, void *priv,
			  bpf_program_clear_priv_t clear_priv)
{
	if (prog->priv && prog->clear_priv)
		prog->clear_priv(prog, prog->priv);

	prog->priv = priv;
	prog->clear_priv = clear_priv;
	return 0;
}

void *bpf_program__priv(struct bpf_program *prog)
{
	return prog ? prog->priv : ERR_PTR(-EINVAL);
}

void bpf_program__set_ifindex(struct bpf_program *prog, __u32 ifindex)
{
	prog->prog_ifindex = ifindex;
}

const char *bpf_program__title(struct bpf_program *prog, bool needs_copy)
{
	const char *title;

	title = prog->section_name;
	if (needs_copy) {
		title = strdup(title);
		if (!title) {
			pr_warning("failed to strdup program title\n");
			return ERR_PTR(-ENOMEM);
		}
	}

	return title;
}

int bpf_program__fd(struct bpf_program *prog)
{
	return bpf_program__nth_fd(prog, 0);
}

int bpf_program__set_prep(struct bpf_program *prog, int nr_instances,
			  bpf_program_prep_t prep)
{
	int *instances_fds;

	if (nr_instances <= 0 || !prep)
		return -EINVAL;

	if (prog->instances.nr > 0 || prog->instances.fds) {
		pr_warning("Can't set pre-processor after loading\n");
		return -EINVAL;
	}

	instances_fds = malloc(sizeof(int) * nr_instances);
	if (!instances_fds) {
		pr_warning("alloc memory failed for fds\n");
		return -ENOMEM;
	}

	/* fill all fd with -1 */
	memset(instances_fds, -1, sizeof(int) * nr_instances);

	prog->instances.nr = nr_instances;
	prog->instances.fds = instances_fds;
	prog->preprocessor = prep;
	return 0;
}

int bpf_program__nth_fd(struct bpf_program *prog, int n)
{
	int fd;

	if (!prog)
		return -EINVAL;

	if (n >= prog->instances.nr || n < 0) {
		pr_warning("Can't get the %dth fd from program %s: only %d instances\n",
			   n, prog->section_name, prog->instances.nr);
		return -EINVAL;
	}

	fd = prog->instances.fds[n];
	if (fd < 0) {
		pr_warning("%dth instance of program '%s' is invalid\n",
			   n, prog->section_name);
		return -ENOENT;
	}

	return fd;
}

void bpf_program__set_type(struct bpf_program *prog, enum bpf_prog_type type)
{
	prog->type = type;
}

static bool bpf_program__is_type(struct bpf_program *prog,
				 enum bpf_prog_type type)
{
	return prog ? (prog->type == type) : false;
}

#define BPF_PROG_TYPE_FNS(NAME, TYPE)			\
int bpf_program__set_##NAME(struct bpf_program *prog)	\
{							\
	if (!prog)					\
		return -EINVAL;				\
	bpf_program__set_type(prog, TYPE);		\
	return 0;					\
}							\
							\
bool bpf_program__is_##NAME(struct bpf_program *prog)	\
{							\
	return bpf_program__is_type(prog, TYPE);	\
}							\

BPF_PROG_TYPE_FNS(socket_filter, BPF_PROG_TYPE_SOCKET_FILTER);
BPF_PROG_TYPE_FNS(kprobe, BPF_PROG_TYPE_KPROBE);
BPF_PROG_TYPE_FNS(sched_cls, BPF_PROG_TYPE_SCHED_CLS);
BPF_PROG_TYPE_FNS(sched_act, BPF_PROG_TYPE_SCHED_ACT);
BPF_PROG_TYPE_FNS(tracepoint, BPF_PROG_TYPE_TRACEPOINT);
BPF_PROG_TYPE_FNS(raw_tracepoint, BPF_PROG_TYPE_RAW_TRACEPOINT);
BPF_PROG_TYPE_FNS(xdp, BPF_PROG_TYPE_XDP);
BPF_PROG_TYPE_FNS(perf_event, BPF_PROG_TYPE_PERF_EVENT);

void bpf_program__set_expected_attach_type(struct bpf_program *prog,
					   enum bpf_attach_type type)
{
	prog->expected_attach_type = type;
}

#define BPF_PROG_SEC_FULL(string, ptype, atype) \
	{ string, sizeof(string) - 1, ptype, atype }

#define BPF_PROG_SEC(string, ptype) BPF_PROG_SEC_FULL(string, ptype, 0)

#define BPF_S_PROG_SEC(string, ptype) \
	BPF_PROG_SEC_FULL(string, BPF_PROG_TYPE_CGROUP_SOCK, ptype)

#define BPF_SA_PROG_SEC(string, ptype) \
	BPF_PROG_SEC_FULL(string, BPF_PROG_TYPE_CGROUP_SOCK_ADDR, ptype)

static const struct {
	const char *sec;
	size_t len;
	enum bpf_prog_type prog_type;
	enum bpf_attach_type expected_attach_type;
} section_names[] = {
	BPF_PROG_SEC("socket",		BPF_PROG_TYPE_SOCKET_FILTER),
	BPF_PROG_SEC("kprobe/",		BPF_PROG_TYPE_KPROBE),
	BPF_PROG_SEC("kretprobe/",	BPF_PROG_TYPE_KPROBE),
	BPF_PROG_SEC("classifier",	BPF_PROG_TYPE_SCHED_CLS),
	BPF_PROG_SEC("action",		BPF_PROG_TYPE_SCHED_ACT),
	BPF_PROG_SEC("tracepoint/",	BPF_PROG_TYPE_TRACEPOINT),
	BPF_PROG_SEC("raw_tracepoint/",	BPF_PROG_TYPE_RAW_TRACEPOINT),
	BPF_PROG_SEC("xdp",		BPF_PROG_TYPE_XDP),
	BPF_PROG_SEC("perf_event",	BPF_PROG_TYPE_PERF_EVENT),
	BPF_PROG_SEC("cgroup/skb",	BPF_PROG_TYPE_CGROUP_SKB),
	BPF_PROG_SEC("cgroup/sock",	BPF_PROG_TYPE_CGROUP_SOCK),
	BPF_PROG_SEC("cgroup/dev",	BPF_PROG_TYPE_CGROUP_DEVICE),
	BPF_PROG_SEC("lwt_in",		BPF_PROG_TYPE_LWT_IN),
	BPF_PROG_SEC("lwt_out",		BPF_PROG_TYPE_LWT_OUT),
	BPF_PROG_SEC("lwt_xmit",	BPF_PROG_TYPE_LWT_XMIT),
	BPF_PROG_SEC("lwt_seg6local",	BPF_PROG_TYPE_LWT_SEG6LOCAL),
	BPF_PROG_SEC("sockops",		BPF_PROG_TYPE_SOCK_OPS),
	BPF_PROG_SEC("sk_skb",		BPF_PROG_TYPE_SK_SKB),
	BPF_PROG_SEC("sk_msg",		BPF_PROG_TYPE_SK_MSG),
	BPF_PROG_SEC("lirc_mode2",	BPF_PROG_TYPE_LIRC_MODE2),
	BPF_SA_PROG_SEC("cgroup/bind4",	BPF_CGROUP_INET4_BIND),
	BPF_SA_PROG_SEC("cgroup/bind6",	BPF_CGROUP_INET6_BIND),
	BPF_SA_PROG_SEC("cgroup/connect4", BPF_CGROUP_INET4_CONNECT),
	BPF_SA_PROG_SEC("cgroup/connect6", BPF_CGROUP_INET6_CONNECT),
	BPF_SA_PROG_SEC("cgroup/sendmsg4", BPF_CGROUP_UDP4_SENDMSG),
	BPF_SA_PROG_SEC("cgroup/sendmsg6", BPF_CGROUP_UDP6_SENDMSG),
	BPF_S_PROG_SEC("cgroup/post_bind4", BPF_CGROUP_INET4_POST_BIND),
	BPF_S_PROG_SEC("cgroup/post_bind6", BPF_CGROUP_INET6_POST_BIND),
};

#undef BPF_PROG_SEC
#undef BPF_PROG_SEC_FULL
#undef BPF_S_PROG_SEC
#undef BPF_SA_PROG_SEC

int libbpf_prog_type_by_name(const char *name, enum bpf_prog_type *prog_type,
			     enum bpf_attach_type *expected_attach_type)
{
	const struct bpf_sec_def *sec_def;
	char *type_names;

	if (!name)
		return -EINVAL;

	sec_def = find_sec_def(name);
	if (sec_def) {
		*prog_type = sec_def->prog_type;
		*expected_attach_type = sec_def->expected_attach_type;
		return 0;
	}

	pr_debug("failed to guess program type from ELF section '%s'\n", name);
	type_names = libbpf_get_type_names(false);
	if (type_names != NULL) {
		pr_debug("supported section(type) names are:%s\n", type_names);
		free(type_names);
	}

	return -ESRCH;
}

static struct bpf_map *find_struct_ops_map_by_offset(struct bpf_object *obj,
						     size_t offset)
{
	struct bpf_map *map;
	size_t i;

	for (i = 0; i < obj->nr_maps; i++) {
		map = &obj->maps[i];
		if (!bpf_map__is_struct_ops(map))
			continue;
		if (map->sec_offset <= offset &&
		    offset - map->sec_offset < map->def.value_size)
			return map;
	}

	return NULL;
}

/* Collect the reloc from ELF and populate the st_ops->progs[] */
static int bpf_object__collect_st_ops_relos(struct bpf_object *obj,
					    GElf_Shdr *shdr, Elf_Data *data)
{
	const struct btf_member *member;
	struct bpf_struct_ops *st_ops;
	struct bpf_program *prog;
	unsigned int shdr_idx;
	const struct btf *btf;
	struct bpf_map *map;
	Elf_Data *symbols;
	unsigned int moff;
	const char *name;
	__u32 member_idx;
	GElf_Sym sym;
	GElf_Rel rel;
	int i, nrels;

	symbols = obj->efile.symbols;
	btf = obj->btf;
	nrels = shdr->sh_size / shdr->sh_entsize;
	for (i = 0; i < nrels; i++) {
		if (!gelf_getrel(data, i, &rel)) {
			pr_warn("struct_ops reloc: failed to get %d reloc\n", i);
			return -LIBBPF_ERRNO__FORMAT;
		}

		if (!gelf_getsym(symbols, GELF_R_SYM(rel.r_info), &sym)) {
			pr_warn("struct_ops reloc: symbol %zx not found\n",
				(size_t)GELF_R_SYM(rel.r_info));
			return -LIBBPF_ERRNO__FORMAT;
		}

		name = elf_strptr(obj->efile.elf, obj->efile.strtabidx,
				  sym.st_name) ? : "<?>";
		map = find_struct_ops_map_by_offset(obj, rel.r_offset);
		if (!map) {
			pr_warn("struct_ops reloc: cannot find map at rel.r_offset %zu\n",
				(size_t)rel.r_offset);
			return -EINVAL;
		}

		moff = rel.r_offset - map->sec_offset;
		shdr_idx = sym.st_shndx;
		st_ops = map->st_ops;
		pr_debug("struct_ops reloc %s: for %lld value %lld shdr_idx %u rel.r_offset %zu map->sec_offset %zu name %d (\'%s\')\n",
			 map->name,
			 (long long)(rel.r_info >> 32),
			 (long long)sym.st_value,
			 shdr_idx, (size_t)rel.r_offset,
			 map->sec_offset, sym.st_name, name);

		if (shdr_idx >= SHN_LORESERVE) {
			pr_warn("struct_ops reloc %s: rel.r_offset %zu shdr_idx %u unsupported non-static function\n",
				map->name, (size_t)rel.r_offset, shdr_idx);
			return -LIBBPF_ERRNO__RELOC;
		}

		member = find_member_by_offset(st_ops->type, moff * 8);
		if (!member) {
			pr_warn("struct_ops reloc %s: cannot find member at moff %u\n",
				map->name, moff);
			return -EINVAL;
		}
		member_idx = member - btf_members(st_ops->type);
		name = btf__name_by_offset(btf, member->name_off);

		if (!resolve_func_ptr(btf, member->type, NULL)) {
			pr_warn("struct_ops reloc %s: cannot relocate non func ptr %s\n",
				map->name, name);
			return -EINVAL;
		}

		prog = bpf_object__find_prog_by_idx(obj, shdr_idx);
		if (!prog) {
			pr_warn("struct_ops reloc %s: cannot find prog at shdr_idx %u to relocate func ptr %s\n",
				map->name, shdr_idx, name);
			return -EINVAL;
		}

		if (prog->type == BPF_PROG_TYPE_UNSPEC) {
			const struct bpf_sec_def *sec_def;

			sec_def = find_sec_def(prog->section_name);
			if (sec_def &&
			    sec_def->prog_type != BPF_PROG_TYPE_STRUCT_OPS) {
				/* for pr_warn */
				prog->type = sec_def->prog_type;
				goto invalid_prog;
			}

			prog->type = BPF_PROG_TYPE_STRUCT_OPS;
			prog->attach_btf_id = st_ops->type_id;
			prog->expected_attach_type = member_idx;
		} else if (prog->type != BPF_PROG_TYPE_STRUCT_OPS ||
			   prog->attach_btf_id != st_ops->type_id ||
			   prog->expected_attach_type != member_idx) {
			goto invalid_prog;
		}
		st_ops->progs[member_idx] = prog;
	}

	return 0;

invalid_prog:
	pr_warn("struct_ops reloc %s: cannot use prog %s in sec %s with type %u attach_btf_id %u expected_attach_type %u for func ptr %s\n",
		map->name, prog->name, prog->section_name, prog->type,
		prog->attach_btf_id, prog->expected_attach_type, name);
	return -EINVAL;
}

#define BTF_TRACE_PREFIX "btf_trace_"
#define BTF_LSM_PREFIX "bpf_lsm_"
#define BTF_ITER_PREFIX "bpf_iter_"
#define BTF_MAX_NAME_SIZE 128

static int find_btf_by_prefix_kind(const struct btf *btf, const char *prefix,
				   const char *name, __u32 kind)
{
	char btf_type_name[BTF_MAX_NAME_SIZE];
	int ret;

	ret = snprintf(btf_type_name, sizeof(btf_type_name),
		       "%s%s", prefix, name);
	/* snprintf returns the number of characters written excluding the
	 * the terminating null. So, if >= BTF_MAX_NAME_SIZE are written, it
	 * indicates truncation.
	 */
	if (ret < 0 || ret >= sizeof(btf_type_name))
		return -ENAMETOOLONG;
	return btf__find_by_name_kind(btf, btf_type_name, kind);
}

static inline int __find_vmlinux_btf_id(struct btf *btf, const char *name,
					enum bpf_attach_type attach_type)
{
	int err;

	if (attach_type == BPF_TRACE_RAW_TP)
		err = find_btf_by_prefix_kind(btf, BTF_TRACE_PREFIX, name,
					      BTF_KIND_TYPEDEF);
	else if (attach_type == BPF_LSM_MAC)
		err = find_btf_by_prefix_kind(btf, BTF_LSM_PREFIX, name,
					      BTF_KIND_FUNC);
	else if (attach_type == BPF_TRACE_ITER)
		err = find_btf_by_prefix_kind(btf, BTF_ITER_PREFIX, name,
					      BTF_KIND_FUNC);
	else
		err = btf__find_by_name_kind(btf, name, BTF_KIND_FUNC);

	if (err <= 0)
		pr_warn("%s is not found in vmlinux BTF\n", name);

	return err;
}

int libbpf_find_vmlinux_btf_id(const char *name,
			       enum bpf_attach_type attach_type)
{
	struct btf *btf;
	int err;

	btf = libbpf_find_kernel_btf();
	if (IS_ERR(btf)) {
		pr_warn("vmlinux BTF is not found\n");
		return -EINVAL;
	}

	err = __find_vmlinux_btf_id(btf, name, attach_type);
	btf__free(btf);
	return err;
}

static int libbpf_find_prog_btf_id(const char *name, __u32 attach_prog_fd)
{
	struct bpf_prog_info_linear *info_linear;
	struct bpf_prog_info *info;
	struct btf *btf = NULL;
	int err = -EINVAL;

	info_linear = bpf_program__get_prog_info_linear(attach_prog_fd, 0);
	if (IS_ERR_OR_NULL(info_linear)) {
		pr_warn("failed get_prog_info_linear for FD %d\n",
			attach_prog_fd);
		return -EINVAL;
	}
	info = &info_linear->info;
	if (!info->btf_id) {
		pr_warn("The target program doesn't have BTF\n");
		goto out;
	}
	if (btf__get_from_id(info->btf_id, &btf)) {
		pr_warn("Failed to get BTF of the program\n");
		goto out;
	}
	err = btf__find_by_name_kind(btf, name, BTF_KIND_FUNC);
	btf__free(btf);
	if (err <= 0) {
		pr_warn("%s is not found in prog's BTF\n", name);
		goto out;
	}
out:
	free(info_linear);
	return err;
}

static int libbpf_find_attach_btf_id(struct bpf_program *prog)
{
	enum bpf_attach_type attach_type = prog->expected_attach_type;
	__u32 attach_prog_fd = prog->attach_prog_fd;
	const char *name = prog->section_name;
	int i, err;

	if (!name)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(section_defs); i++) {
		if (!section_defs[i].is_attach_btf)
			continue;
		if (strncmp(name, section_defs[i].sec, section_defs[i].len))
			continue;
		if (attach_prog_fd)
			err = libbpf_find_prog_btf_id(name + section_defs[i].len,
						      attach_prog_fd);
		else
			err = __find_vmlinux_btf_id(prog->obj->btf_vmlinux,
						    name + section_defs[i].len,
						    attach_type);
		return err;
	}
	pr_warn("failed to identify btf_id based on ELF section name '%s'\n", name);
	return -ESRCH;
}

int libbpf_attach_type_by_name(const char *name,
			       enum bpf_attach_type *attach_type)
{
	char *type_names;
	int i;

	if (!name)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(section_names); i++) {
		if (strncmp(name, section_names[i].sec, section_names[i].len))
			continue;
		*prog_type = section_names[i].prog_type;
		*expected_attach_type = section_names[i].expected_attach_type;
		return 0;
	}
	return -EINVAL;
}

static int
bpf_program__identify_section(struct bpf_program *prog,
			      enum bpf_prog_type *prog_type,
			      enum bpf_attach_type *expected_attach_type)
{
	return libbpf_prog_type_by_name(prog->section_name, prog_type,
					expected_attach_type);
}

int bpf_map__fd(struct bpf_map *map)
{
	return map ? map->fd : -EINVAL;
}

const struct bpf_map_def *bpf_map__def(struct bpf_map *map)
{
	return map ? &map->def : ERR_PTR(-EINVAL);
}

const char *bpf_map__name(struct bpf_map *map)
{
	return map ? map->name : NULL;
}

__u32 bpf_map__btf_key_type_id(const struct bpf_map *map)
{
	return map ? map->btf_key_type_id : 0;
}

__u32 bpf_map__btf_value_type_id(const struct bpf_map *map)
{
	return map ? map->btf_value_type_id : 0;
}

int bpf_map__set_priv(struct bpf_map *map, void *priv,
		     bpf_map_clear_priv_t clear_priv)
{
	if (!map)
		return -EINVAL;

	if (map->priv) {
		if (map->clear_priv)
			map->clear_priv(map, map->priv);
	}

	map->priv = priv;
	map->clear_priv = clear_priv;
	return 0;
}

void *bpf_map__priv(struct bpf_map *map)
{
	return map ? map->priv : ERR_PTR(-EINVAL);
}

bool bpf_map__is_offload_neutral(struct bpf_map *map)
{
	return map->def.type == BPF_MAP_TYPE_PERF_EVENT_ARRAY;
}

void bpf_map__set_ifindex(struct bpf_map *map, __u32 ifindex)
{
	map->map_ifindex = ifindex;
}

struct bpf_map *
bpf_map__next(struct bpf_map *prev, struct bpf_object *obj)
{
	size_t idx;
	struct bpf_map *s, *e;

	if (!obj || !obj->maps)
		return NULL;

	s = obj->maps;
	e = obj->maps + obj->nr_maps;

	if (prev == NULL)
		return s;

	if ((prev < s) || (prev >= e)) {
		pr_warning("error in %s: map handler doesn't belong to object\n",
			   __func__);
		return NULL;
	}

	idx = (prev - obj->maps) + 1;
	if (idx >= obj->nr_maps)
		return NULL;
	return &obj->maps[idx];
}

struct bpf_map *
bpf_object__find_map_by_name(struct bpf_object *obj, const char *name)
{
	struct bpf_map *pos;

	bpf_map__for_each(pos, obj) {
		if (pos->name && !strcmp(pos->name, name))
			return pos;
	}
	return NULL;
}

struct bpf_map *
bpf_object__find_map_by_offset(struct bpf_object *obj, size_t offset)
{
	int i;

	for (i = 0; i < obj->nr_maps; i++) {
		if (obj->maps[i].offset == offset)
			return &obj->maps[i];
	}
	return ERR_PTR(-ENOENT);
}

long libbpf_get_error(const void *ptr)
{
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);
	return 0;
}

int bpf_prog_load(const char *file, enum bpf_prog_type type,
		  struct bpf_object **pobj, int *prog_fd)
{
	struct bpf_prog_load_attr attr;

	memset(&attr, 0, sizeof(struct bpf_prog_load_attr));
	attr.file = file;
	attr.prog_type = type;
	attr.expected_attach_type = 0;

	return bpf_prog_load_xattr(&attr, pobj, prog_fd);
}

int bpf_prog_load_xattr(const struct bpf_prog_load_attr *attr,
			struct bpf_object **pobj, int *prog_fd)
{
	struct bpf_object_open_attr open_attr = {};
	struct bpf_program *prog, *first_prog = NULL;
	enum bpf_attach_type expected_attach_type;
	enum bpf_prog_type prog_type;
	struct bpf_object *obj;
	struct bpf_map *map;
	int err;

	if (!attr)
		return -EINVAL;
	if (!attr->file)
		return -EINVAL;

	open_attr.file = attr->file;
	open_attr.prog_type = attr->prog_type;

	obj = bpf_object__open_xattr(&open_attr);
	if (IS_ERR_OR_NULL(obj))
		return -ENOENT;

	bpf_object__for_each_program(prog, obj) {
		/*
		 * If type is not specified, try to guess it based on
		 * section name.
		 */
		prog_type = attr->prog_type;
		prog->prog_ifindex = attr->ifindex;
		expected_attach_type = attr->expected_attach_type;
		if (prog_type == BPF_PROG_TYPE_UNSPEC) {
			err = bpf_program__identify_section(prog, &prog_type,
							    &expected_attach_type);
			if (err < 0) {
				pr_warning("failed to guess program type based on section name %s\n",
					   prog->section_name);
				bpf_object__close(obj);
				return -EINVAL;
			}
		}

		bpf_program__set_type(prog, prog_type);
		bpf_program__set_expected_attach_type(prog,
						      expected_attach_type);

		if (!bpf_program__is_function_storage(prog, obj) && !first_prog)
			first_prog = prog;
	}

	bpf_map__for_each(map, obj) {
		if (!bpf_map__is_offload_neutral(map))
			map->map_ifindex = attr->ifindex;
	}

	if (!first_prog) {
		pr_warning("object file doesn't contain bpf program\n");
		bpf_object__close(obj);
		return -ENOENT;
	}

	err = bpf_object__load(obj);
	if (err) {
		bpf_object__close(obj);
		return -EINVAL;
	}

	*pobj = obj;
	*prog_fd = bpf_program__fd(first_prog);
	return 0;
}

enum bpf_perf_event_ret
bpf_perf_event_read_simple(void *mem, unsigned long size,
			   unsigned long page_size, void **buf, size_t *buf_len,
			   bpf_perf_event_print_t fn, void *priv)
{
	volatile struct perf_event_mmap_page *header = mem;
	__u64 data_tail = header->data_tail;
	__u64 data_head = header->data_head;
	int ret = LIBBPF_PERF_EVENT_ERROR;
	void *base, *begin, *end;

	asm volatile("" ::: "memory"); /* in real code it should be smp_rmb() */
	if (data_head == data_tail)
		return LIBBPF_PERF_EVENT_CONT;

	base = ((char *)header) + page_size;

	begin = base + data_tail % size;
	end = base + data_head % size;

	while (begin != end) {
		struct perf_event_header *ehdr;

		ehdr = begin;
		if (begin + ehdr->size > base + size) {
			long len = base + size - begin;

			if (*buf_len < ehdr->size) {
				free(*buf);
				*buf = malloc(ehdr->size);
				if (!*buf) {
					ret = LIBBPF_PERF_EVENT_ERROR;
					break;
				}
				*buf_len = ehdr->size;
			}

			memcpy(*buf, begin, len);
			memcpy(*buf + len, base, ehdr->size - len);
			ehdr = (void *)*buf;
			begin = base + ehdr->size - len;
		} else if (begin + ehdr->size == base + size) {
			begin = base;
		} else {
			begin += ehdr->size;
		}

		ret = fn(ehdr, priv);
		if (ret != LIBBPF_PERF_EVENT_CONT)
			break;

		data_tail += ehdr->size;
	}

	__sync_synchronize(); /* smp_mb() */
	header->data_tail = data_tail;

	return ret;
}

struct perf_buffer;

struct perf_buffer_params {
	struct perf_event_attr *attr;
	/* if event_cb is specified, it takes precendence */
	perf_buffer_event_fn event_cb;
	/* sample_cb and lost_cb are higher-level common-case callbacks */
	perf_buffer_sample_fn sample_cb;
	perf_buffer_lost_fn lost_cb;
	void *ctx;
	int cpu_cnt;
	int *cpus;
	int *map_keys;
};

struct perf_cpu_buf {
	struct perf_buffer *pb;
	void *base; /* mmap()'ed memory */
	void *buf; /* for reconstructing segmented data */
	size_t buf_size;
	int fd;
	int cpu;
	int map_key;
};

struct perf_buffer {
	perf_buffer_event_fn event_cb;
	perf_buffer_sample_fn sample_cb;
	perf_buffer_lost_fn lost_cb;
	void *ctx; /* passed into callbacks */

	size_t page_size;
	size_t mmap_size;
	struct perf_cpu_buf **cpu_bufs;
	struct epoll_event *events;
	int cpu_cnt; /* number of allocated CPU buffers */
	int epoll_fd; /* perf event FD */
	int map_fd; /* BPF_MAP_TYPE_PERF_EVENT_ARRAY BPF map FD */
};

static void perf_buffer__free_cpu_buf(struct perf_buffer *pb,
				      struct perf_cpu_buf *cpu_buf)
{
	if (!cpu_buf)
		return;
	if (cpu_buf->base &&
	    munmap(cpu_buf->base, pb->mmap_size + pb->page_size))
		pr_warn("failed to munmap cpu_buf #%d\n", cpu_buf->cpu);
	if (cpu_buf->fd >= 0) {
		ioctl(cpu_buf->fd, PERF_EVENT_IOC_DISABLE, 0);
		close(cpu_buf->fd);
	}
	free(cpu_buf->buf);
	free(cpu_buf);
}

void perf_buffer__free(struct perf_buffer *pb)
{
	int i;

	if (!pb)
		return;
	if (pb->cpu_bufs) {
		for (i = 0; i < pb->cpu_cnt && pb->cpu_bufs[i]; i++) {
			struct perf_cpu_buf *cpu_buf = pb->cpu_bufs[i];

			bpf_map_delete_elem(pb->map_fd, &cpu_buf->map_key);
			perf_buffer__free_cpu_buf(pb, cpu_buf);
		}
		free(pb->cpu_bufs);
	}
	if (pb->epoll_fd >= 0)
		close(pb->epoll_fd);
	free(pb->events);
	free(pb);
}

static struct perf_cpu_buf *
perf_buffer__open_cpu_buf(struct perf_buffer *pb, struct perf_event_attr *attr,
			  int cpu, int map_key)
{
	struct perf_cpu_buf *cpu_buf;
	char msg[STRERR_BUFSIZE];
	int err;

	cpu_buf = calloc(1, sizeof(*cpu_buf));
	if (!cpu_buf)
		return ERR_PTR(-ENOMEM);

	cpu_buf->pb = pb;
	cpu_buf->cpu = cpu;
	cpu_buf->map_key = map_key;

	cpu_buf->fd = syscall(__NR_perf_event_open, attr, -1 /* pid */, cpu,
			      -1, PERF_FLAG_FD_CLOEXEC);
	if (cpu_buf->fd < 0) {
		err = -errno;
		pr_warn("failed to open perf buffer event on cpu #%d: %s\n",
			cpu, libbpf_strerror_r(err, msg, sizeof(msg)));
		goto error;
	}

	cpu_buf->base = mmap(NULL, pb->mmap_size + pb->page_size,
			     PROT_READ | PROT_WRITE, MAP_SHARED,
			     cpu_buf->fd, 0);
	if (cpu_buf->base == MAP_FAILED) {
		cpu_buf->base = NULL;
		err = -errno;
		pr_warn("failed to mmap perf buffer on cpu #%d: %s\n",
			cpu, libbpf_strerror_r(err, msg, sizeof(msg)));
		goto error;
	}

	if (ioctl(cpu_buf->fd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
		err = -errno;
		pr_warn("failed to enable perf buffer event on cpu #%d: %s\n",
			cpu, libbpf_strerror_r(err, msg, sizeof(msg)));
		goto error;
	}

	return cpu_buf;

error:
	perf_buffer__free_cpu_buf(pb, cpu_buf);
	return (struct perf_cpu_buf *)ERR_PTR(err);
}

static struct perf_buffer *__perf_buffer__new(int map_fd, size_t page_cnt,
					      struct perf_buffer_params *p);

struct perf_buffer *perf_buffer__new(int map_fd, size_t page_cnt,
				     const struct perf_buffer_opts *opts)
{
	struct perf_buffer_params p = {};
	struct perf_event_attr attr = { 0, };

	attr.config = PERF_COUNT_SW_BPF_OUTPUT,
	attr.type = PERF_TYPE_SOFTWARE;
	attr.sample_type = PERF_SAMPLE_RAW;
	attr.sample_period = 1;
	attr.wakeup_events = 1;

	p.attr = &attr;
	p.sample_cb = opts ? opts->sample_cb : NULL;
	p.lost_cb = opts ? opts->lost_cb : NULL;
	p.ctx = opts ? opts->ctx : NULL;

	return __perf_buffer__new(map_fd, page_cnt, &p);
}

struct perf_buffer *
perf_buffer__new_raw(int map_fd, size_t page_cnt,
		     const struct perf_buffer_raw_opts *opts)
{
	struct perf_buffer_params p = {};

	p.attr = opts->attr;
	p.event_cb = opts->event_cb;
	p.ctx = opts->ctx;
	p.cpu_cnt = opts->cpu_cnt;
	p.cpus = opts->cpus;
	p.map_keys = opts->map_keys;

	return __perf_buffer__new(map_fd, page_cnt, &p);
}

static struct perf_buffer *__perf_buffer__new(int map_fd, size_t page_cnt,
					      struct perf_buffer_params *p)
{
	const char *online_cpus_file = "/sys/devices/system/cpu/online";
	struct bpf_map_info map = {};
	char msg[STRERR_BUFSIZE];
	struct perf_buffer *pb;
	bool *online = NULL;
	__u32 map_info_len;
	int err, i, j, n;

	if (page_cnt & (page_cnt - 1)) {
		pr_warn("page count should be power of two, but is %zu\n",
			page_cnt);
		return ERR_PTR(-EINVAL);
	}

	map_info_len = sizeof(map);
	err = bpf_obj_get_info_by_fd(map_fd, &map, &map_info_len);
	if (err) {
		err = -errno;
		pr_warn("failed to get map info for map FD %d: %s\n",
			map_fd, libbpf_strerror_r(err, msg, sizeof(msg)));
		return ERR_PTR(err);
	}

	if (map.type != BPF_MAP_TYPE_PERF_EVENT_ARRAY) {
		pr_warn("map '%s' should be BPF_MAP_TYPE_PERF_EVENT_ARRAY\n",
			map.name);
		return ERR_PTR(-EINVAL);
	}

	pb = calloc(1, sizeof(*pb));
	if (!pb)
		return ERR_PTR(-ENOMEM);

	pb->event_cb = p->event_cb;
	pb->sample_cb = p->sample_cb;
	pb->lost_cb = p->lost_cb;
	pb->ctx = p->ctx;

	pb->page_size = getpagesize();
	pb->mmap_size = pb->page_size * page_cnt;
	pb->map_fd = map_fd;

	pb->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (pb->epoll_fd < 0) {
		err = -errno;
		pr_warn("failed to create epoll instance: %s\n",
			libbpf_strerror_r(err, msg, sizeof(msg)));
		goto error;
	}

	if (p->cpu_cnt > 0) {
		pb->cpu_cnt = p->cpu_cnt;
	} else {
		pb->cpu_cnt = libbpf_num_possible_cpus();
		if (pb->cpu_cnt < 0) {
			err = pb->cpu_cnt;
			goto error;
		}
		if (map.max_entries < pb->cpu_cnt)
			pb->cpu_cnt = map.max_entries;
	}

	pb->events = calloc(pb->cpu_cnt, sizeof(*pb->events));
	if (!pb->events) {
		err = -ENOMEM;
		pr_warn("failed to allocate events: out of memory\n");
		goto error;
	}
	pb->cpu_bufs = calloc(pb->cpu_cnt, sizeof(*pb->cpu_bufs));
	if (!pb->cpu_bufs) {
		err = -ENOMEM;
		pr_warn("failed to allocate buffers: out of memory\n");
		goto error;
	}

	err = parse_cpu_mask_file(online_cpus_file, &online, &n);
	if (err) {
		pr_warn("failed to get online CPU mask: %d\n", err);
		goto error;
	}

	for (i = 0, j = 0; i < pb->cpu_cnt; i++) {
		struct perf_cpu_buf *cpu_buf;
		int cpu, map_key;

		cpu = p->cpu_cnt > 0 ? p->cpus[i] : i;
		map_key = p->cpu_cnt > 0 ? p->map_keys[i] : i;

		/* in case user didn't explicitly requested particular CPUs to
		 * be attached to, skip offline/not present CPUs
		 */
		if (p->cpu_cnt <= 0 && (cpu >= n || !online[cpu]))
			continue;

		cpu_buf = perf_buffer__open_cpu_buf(pb, p->attr, cpu, map_key);
		if (IS_ERR(cpu_buf)) {
			err = PTR_ERR(cpu_buf);
			goto error;
		}

		pb->cpu_bufs[j] = cpu_buf;

		err = bpf_map_update_elem(pb->map_fd, &map_key,
					  &cpu_buf->fd, 0);
		if (err) {
			err = -errno;
			pr_warn("failed to set cpu #%d, key %d -> perf FD %d: %s\n",
				cpu, map_key, cpu_buf->fd,
				libbpf_strerror_r(err, msg, sizeof(msg)));
			goto error;
		}

		pb->events[j].events = EPOLLIN;
		pb->events[j].data.ptr = cpu_buf;
		if (epoll_ctl(pb->epoll_fd, EPOLL_CTL_ADD, cpu_buf->fd,
			      &pb->events[j]) < 0) {
			err = -errno;
			pr_warn("failed to epoll_ctl cpu #%d perf FD %d: %s\n",
				cpu, cpu_buf->fd,
				libbpf_strerror_r(err, msg, sizeof(msg)));
			goto error;
		}
		j++;
	}
	pb->cpu_cnt = j;
	free(online);

	return pb;

error:
	free(online);
	if (pb)
		perf_buffer__free(pb);
	return ERR_PTR(err);
}

struct perf_sample_raw {
	struct perf_event_header header;
	uint32_t size;
	char data[];
};

struct perf_sample_lost {
	struct perf_event_header header;
	uint64_t id;
	uint64_t lost;
	uint64_t sample_id;
};

static enum bpf_perf_event_ret
perf_buffer__process_record(struct perf_event_header *e, void *ctx)
{
	struct perf_cpu_buf *cpu_buf = ctx;
	struct perf_buffer *pb = cpu_buf->pb;
	void *data = e;

	/* user wants full control over parsing perf event */
	if (pb->event_cb)
		return pb->event_cb(pb->ctx, cpu_buf->cpu, e);

	switch (e->type) {
	case PERF_RECORD_SAMPLE: {
		struct perf_sample_raw *s = data;

		if (pb->sample_cb)
			pb->sample_cb(pb->ctx, cpu_buf->cpu, s->data, s->size);
		break;
	}
	case PERF_RECORD_LOST: {
		struct perf_sample_lost *s = data;

		if (pb->lost_cb)
			pb->lost_cb(pb->ctx, cpu_buf->cpu, s->lost);
		break;
	}
	default:
		pr_warn("unknown perf sample type %d\n", e->type);
		return LIBBPF_PERF_EVENT_ERROR;
	}
	return LIBBPF_PERF_EVENT_CONT;
}

static int perf_buffer__process_records(struct perf_buffer *pb,
					struct perf_cpu_buf *cpu_buf)
{
	enum bpf_perf_event_ret ret;

	ret = bpf_perf_event_read_simple(cpu_buf->base, pb->mmap_size,
					 pb->page_size, &cpu_buf->buf,
					 &cpu_buf->buf_size,
					 perf_buffer__process_record, cpu_buf);
	if (ret != LIBBPF_PERF_EVENT_CONT)
		return ret;
	return 0;
}

int perf_buffer__poll(struct perf_buffer *pb, int timeout_ms)
{
	int i, cnt, err;

	cnt = epoll_wait(pb->epoll_fd, pb->events, pb->cpu_cnt, timeout_ms);
	for (i = 0; i < cnt; i++) {
		struct perf_cpu_buf *cpu_buf = pb->events[i].data.ptr;

		err = perf_buffer__process_records(pb, cpu_buf);
		if (err) {
			pr_warn("error while processing records: %d\n", err);
			return err;
		}
	}
	return cnt < 0 ? -errno : cnt;
}

struct bpf_prog_info_array_desc {
	int	array_offset;	/* e.g. offset of jited_prog_insns */
	int	count_offset;	/* e.g. offset of jited_prog_len */
	int	size_offset;	/* > 0: offset of rec size,
				 * < 0: fix size of -size_offset
				 */
};

static struct bpf_prog_info_array_desc bpf_prog_info_array_desc[] = {
	[BPF_PROG_INFO_JITED_INSNS] = {
		offsetof(struct bpf_prog_info, jited_prog_insns),
		offsetof(struct bpf_prog_info, jited_prog_len),
		-1,
	},
	[BPF_PROG_INFO_XLATED_INSNS] = {
		offsetof(struct bpf_prog_info, xlated_prog_insns),
		offsetof(struct bpf_prog_info, xlated_prog_len),
		-1,
	},
	[BPF_PROG_INFO_MAP_IDS] = {
		offsetof(struct bpf_prog_info, map_ids),
		offsetof(struct bpf_prog_info, nr_map_ids),
		-(int)sizeof(__u32),
	},
	[BPF_PROG_INFO_JITED_KSYMS] = {
		offsetof(struct bpf_prog_info, jited_ksyms),
		offsetof(struct bpf_prog_info, nr_jited_ksyms),
		-(int)sizeof(__u64),
	},
	[BPF_PROG_INFO_JITED_FUNC_LENS] = {
		offsetof(struct bpf_prog_info, jited_func_lens),
		offsetof(struct bpf_prog_info, nr_jited_func_lens),
		-(int)sizeof(__u32),
	},
	[BPF_PROG_INFO_FUNC_INFO] = {
		offsetof(struct bpf_prog_info, func_info),
		offsetof(struct bpf_prog_info, nr_func_info),
		offsetof(struct bpf_prog_info, func_info_rec_size),
	},
	[BPF_PROG_INFO_LINE_INFO] = {
		offsetof(struct bpf_prog_info, line_info),
		offsetof(struct bpf_prog_info, nr_line_info),
		offsetof(struct bpf_prog_info, line_info_rec_size),
	},
	[BPF_PROG_INFO_JITED_LINE_INFO] = {
		offsetof(struct bpf_prog_info, jited_line_info),
		offsetof(struct bpf_prog_info, nr_jited_line_info),
		offsetof(struct bpf_prog_info, jited_line_info_rec_size),
	},
	[BPF_PROG_INFO_PROG_TAGS] = {
		offsetof(struct bpf_prog_info, prog_tags),
		offsetof(struct bpf_prog_info, nr_prog_tags),
		-(int)sizeof(__u8) * BPF_TAG_SIZE,
	},

};

static __u32 bpf_prog_info_read_offset_u32(struct bpf_prog_info *info,
					   int offset)
{
	__u32 *array = (__u32 *)info;

	if (offset >= 0)
		return array[offset / sizeof(__u32)];
	return -(int)offset;
}

static __u64 bpf_prog_info_read_offset_u64(struct bpf_prog_info *info,
					   int offset)
{
	__u64 *array = (__u64 *)info;

	if (offset >= 0)
		return array[offset / sizeof(__u64)];
	return -(int)offset;
}

static void bpf_prog_info_set_offset_u32(struct bpf_prog_info *info, int offset,
					 __u32 val)
{
	__u32 *array = (__u32 *)info;

	if (offset >= 0)
		array[offset / sizeof(__u32)] = val;
}

static void bpf_prog_info_set_offset_u64(struct bpf_prog_info *info, int offset,
					 __u64 val)
{
	__u64 *array = (__u64 *)info;

	if (offset >= 0)
		array[offset / sizeof(__u64)] = val;
}

struct bpf_prog_info_linear *
bpf_program__get_prog_info_linear(int fd, __u64 arrays)
{
	struct bpf_prog_info_linear *info_linear;
	struct bpf_prog_info info = {};
	__u32 info_len = sizeof(info);
	__u32 data_len = 0;
	int i, err;
	void *ptr;

	if (arrays >> BPF_PROG_INFO_LAST_ARRAY)
		return ERR_PTR(-EINVAL);

	/* step 1: get array dimensions */
	err = bpf_obj_get_info_by_fd(fd, &info, &info_len);
	if (err) {
		pr_debug("can't get prog info: %s", strerror(errno));
		return ERR_PTR(-EFAULT);
	}

	/* step 2: calculate total size of all arrays */
	for (i = BPF_PROG_INFO_FIRST_ARRAY; i < BPF_PROG_INFO_LAST_ARRAY; ++i) {
		bool include_array = (arrays & (1UL << i)) > 0;
		struct bpf_prog_info_array_desc *desc;
		__u32 count, size;

		desc = bpf_prog_info_array_desc + i;

		/* kernel is too old to support this field */
		if (info_len < desc->array_offset + sizeof(__u32) ||
		    info_len < desc->count_offset + sizeof(__u32) ||
		    (desc->size_offset > 0 && info_len < desc->size_offset))
			include_array = false;

		if (!include_array) {
			arrays &= ~(1UL << i);	/* clear the bit */
			continue;
		}

		count = bpf_prog_info_read_offset_u32(&info, desc->count_offset);
		size  = bpf_prog_info_read_offset_u32(&info, desc->size_offset);

		data_len += count * size;
	}

	/* step 3: allocate continuous memory */
	data_len = roundup(data_len, sizeof(__u64));
	info_linear = malloc(sizeof(struct bpf_prog_info_linear) + data_len);
	if (!info_linear)
		return ERR_PTR(-ENOMEM);

	/* step 4: fill data to info_linear->info */
	info_linear->arrays = arrays;
	memset(&info_linear->info, 0, sizeof(info));
	ptr = info_linear->data;

	for (i = BPF_PROG_INFO_FIRST_ARRAY; i < BPF_PROG_INFO_LAST_ARRAY; ++i) {
		struct bpf_prog_info_array_desc *desc;
		__u32 count, size;

		if ((arrays & (1UL << i)) == 0)
			continue;

		desc  = bpf_prog_info_array_desc + i;
		count = bpf_prog_info_read_offset_u32(&info, desc->count_offset);
		size  = bpf_prog_info_read_offset_u32(&info, desc->size_offset);
		bpf_prog_info_set_offset_u32(&info_linear->info,
					     desc->count_offset, count);
		bpf_prog_info_set_offset_u32(&info_linear->info,
					     desc->size_offset, size);
		bpf_prog_info_set_offset_u64(&info_linear->info,
					     desc->array_offset,
					     ptr_to_u64(ptr));
		ptr += count * size;
	}

	/* step 5: call syscall again to get required arrays */
	err = bpf_obj_get_info_by_fd(fd, &info_linear->info, &info_len);
	if (err) {
		pr_debug("can't get prog info: %s", strerror(errno));
		free(info_linear);
		return ERR_PTR(-EFAULT);
	}

	/* step 6: verify the data */
	for (i = BPF_PROG_INFO_FIRST_ARRAY; i < BPF_PROG_INFO_LAST_ARRAY; ++i) {
		struct bpf_prog_info_array_desc *desc;
		__u32 v1, v2;

		if ((arrays & (1UL << i)) == 0)
			continue;

		desc = bpf_prog_info_array_desc + i;
		v1 = bpf_prog_info_read_offset_u32(&info, desc->count_offset);
		v2 = bpf_prog_info_read_offset_u32(&info_linear->info,
						   desc->count_offset);
		if (v1 != v2)
			pr_warn("%s: mismatch in element count\n", __func__);

		v1 = bpf_prog_info_read_offset_u32(&info, desc->size_offset);
		v2 = bpf_prog_info_read_offset_u32(&info_linear->info,
						   desc->size_offset);
		if (v1 != v2)
			pr_warn("%s: mismatch in rec size\n", __func__);
	}

	/* step 7: update info_len and data_len */
	info_linear->info_len = sizeof(struct bpf_prog_info);
	info_linear->data_len = data_len;

	return info_linear;
}

void bpf_program__bpil_addr_to_offs(struct bpf_prog_info_linear *info_linear)
{
	int i;

	for (i = BPF_PROG_INFO_FIRST_ARRAY; i < BPF_PROG_INFO_LAST_ARRAY; ++i) {
		struct bpf_prog_info_array_desc *desc;
		__u64 addr, offs;

		if ((info_linear->arrays & (1UL << i)) == 0)
			continue;

		desc = bpf_prog_info_array_desc + i;
		addr = bpf_prog_info_read_offset_u64(&info_linear->info,
						     desc->array_offset);
		offs = addr - ptr_to_u64(info_linear->data);
		bpf_prog_info_set_offset_u64(&info_linear->info,
					     desc->array_offset, offs);
	}
}

void bpf_program__bpil_offs_to_addr(struct bpf_prog_info_linear *info_linear)
{
	int i;

	for (i = BPF_PROG_INFO_FIRST_ARRAY; i < BPF_PROG_INFO_LAST_ARRAY; ++i) {
		struct bpf_prog_info_array_desc *desc;
		__u64 addr, offs;

		if ((info_linear->arrays & (1UL << i)) == 0)
			continue;

		desc = bpf_prog_info_array_desc + i;
		offs = bpf_prog_info_read_offset_u64(&info_linear->info,
						     desc->array_offset);
		addr = offs + ptr_to_u64(info_linear->data);
		bpf_prog_info_set_offset_u64(&info_linear->info,
					     desc->array_offset, addr);
	}
}

int bpf_program__set_attach_target(struct bpf_program *prog,
				   int attach_prog_fd,
				   const char *attach_func_name)
{
	int btf_id;

	if (!prog || attach_prog_fd < 0 || !attach_func_name)
		return -EINVAL;

	if (attach_prog_fd)
		btf_id = libbpf_find_prog_btf_id(attach_func_name,
						 attach_prog_fd);
	else
		btf_id = __find_vmlinux_btf_id(prog->obj->btf_vmlinux,
					       attach_func_name,
					       prog->expected_attach_type);

	if (btf_id < 0)
		return btf_id;

	prog->attach_btf_id = btf_id;
	prog->attach_prog_fd = attach_prog_fd;
	return 0;
}

int parse_cpu_mask_str(const char *s, bool **mask, int *mask_sz)
{
	int err = 0, n, len, start, end = -1;
	bool *tmp;

	*mask = NULL;
	*mask_sz = 0;

	/* Each sub string separated by ',' has format \d+-\d+ or \d+ */
	while (*s) {
		if (*s == ',' || *s == '\n') {
			s++;
			continue;
		}
		n = sscanf(s, "%d%n-%d%n", &start, &len, &end, &len);
		if (n <= 0 || n > 2) {
			pr_warn("Failed to get CPU range %s: %d\n", s, n);
			err = -EINVAL;
			goto cleanup;
		} else if (n == 1) {
			end = start;
		}
		if (start < 0 || start > end) {
			pr_warn("Invalid CPU range [%d,%d] in %s\n",
				start, end, s);
			err = -EINVAL;
			goto cleanup;
		}
		tmp = realloc(*mask, end + 1);
		if (!tmp) {
			err = -ENOMEM;
			goto cleanup;
		}
		*mask = tmp;
		memset(tmp + *mask_sz, 0, start - *mask_sz);
		memset(tmp + start, 1, end - start + 1);
		*mask_sz = end + 1;
		s += len;
	}
	if (!*mask_sz) {
		pr_warn("Empty CPU range\n");
		return -EINVAL;
	}
	return 0;
cleanup:
	free(*mask);
	*mask = NULL;
	return err;
}

int parse_cpu_mask_file(const char *fcpu, bool **mask, int *mask_sz)
{
	int fd, err = 0, len;
	char buf[128];

	fd = open(fcpu, O_RDONLY);
	if (fd < 0) {
		err = -errno;
		pr_warn("Failed to open cpu mask file %s: %d\n", fcpu, err);
		return err;
	}
	len = read(fd, buf, sizeof(buf));
	close(fd);
	if (len <= 0) {
		err = len ? -errno : -EINVAL;
		pr_warn("Failed to read cpu mask from %s: %d\n", fcpu, err);
		return err;
	}
	if (len >= sizeof(buf)) {
		pr_warn("CPU mask is too big in file %s\n", fcpu);
		return -E2BIG;
	}
	buf[len] = '\0';

	return parse_cpu_mask_str(buf, mask, mask_sz);
}

int libbpf_num_possible_cpus(void)
{
	static const char *fcpu = "/sys/devices/system/cpu/possible";
	static int cpus;
	int err, n, i, tmp_cpus;
	bool *mask;

	tmp_cpus = READ_ONCE(cpus);
	if (tmp_cpus > 0)
		return tmp_cpus;

	err = parse_cpu_mask_file(fcpu, &mask, &n);
	if (err)
		return err;

	tmp_cpus = 0;
	for (i = 0; i < n; i++) {
		if (mask[i])
			tmp_cpus++;
	}
	free(mask);

	WRITE_ONCE(cpus, tmp_cpus);
	return tmp_cpus;
}

int bpf_object__open_skeleton(struct bpf_object_skeleton *s,
			      const struct bpf_object_open_opts *opts)
{
	DECLARE_LIBBPF_OPTS(bpf_object_open_opts, skel_opts,
		.object_name = s->name,
	);
	struct bpf_object *obj;
	int i;

	/* Attempt to preserve opts->object_name, unless overriden by user
	 * explicitly. Overwriting object name for skeletons is discouraged,
	 * as it breaks global data maps, because they contain object name
	 * prefix as their own map name prefix. When skeleton is generated,
	 * bpftool is making an assumption that this name will stay the same.
	 */
	if (opts) {
		memcpy(&skel_opts, opts, sizeof(*opts));
		if (!opts->object_name)
			skel_opts.object_name = s->name;
	}

	obj = bpf_object__open_mem(s->data, s->data_sz, &skel_opts);
	if (IS_ERR(obj)) {
		pr_warn("failed to initialize skeleton BPF object '%s': %ld\n",
			s->name, PTR_ERR(obj));
		return PTR_ERR(obj);
	}

	*s->obj = obj;

	for (i = 0; i < s->map_cnt; i++) {
		struct bpf_map **map = s->maps[i].map;
		const char *name = s->maps[i].name;
		void **mmaped = s->maps[i].mmaped;

		*map = bpf_object__find_map_by_name(obj, name);
		if (!*map) {
			pr_warn("failed to find skeleton map '%s'\n", name);
			return -ESRCH;
		}

		/* externs shouldn't be pre-setup from user code */
		if (mmaped && (*map)->libbpf_type != LIBBPF_MAP_KCONFIG)
			*mmaped = (*map)->mmaped;
	}

	for (i = 0; i < s->prog_cnt; i++) {
		struct bpf_program **prog = s->progs[i].prog;
		const char *name = s->progs[i].name;

		*prog = bpf_object__find_program_by_name(obj, name);
		if (!*prog) {
			pr_warn("failed to find skeleton program '%s'\n", name);
			return -ESRCH;
		}
	}

	return 0;
}

int bpf_object__load_skeleton(struct bpf_object_skeleton *s)
{
	int i, err;

	err = bpf_object__load(*s->obj);
	if (err) {
		pr_warn("failed to load BPF skeleton '%s': %d\n", s->name, err);
		return err;
	}

	for (i = 0; i < s->map_cnt; i++) {
		struct bpf_map *map = *s->maps[i].map;
		size_t mmap_sz = bpf_map_mmap_sz(map);
		int prot, map_fd = bpf_map__fd(map);
		void **mmaped = s->maps[i].mmaped;

		if (!mmaped)
			continue;

		if (!(map->def.map_flags & BPF_F_MMAPABLE)) {
			*mmaped = NULL;
			continue;
		}

		if (map->def.map_flags & BPF_F_RDONLY_PROG)
			prot = PROT_READ;
		else
			prot = PROT_READ | PROT_WRITE;

		/* Remap anonymous mmap()-ed "map initialization image" as
		 * a BPF map-backed mmap()-ed memory, but preserving the same
		 * memory address. This will cause kernel to change process'
		 * page table to point to a different piece of kernel memory,
		 * but from userspace point of view memory address (and its
		 * contents, being identical at this point) will stay the
		 * same. This mapping will be released by bpf_object__close()
		 * as per normal clean up procedure, so we don't need to worry
		 * about it from skeleton's clean up perspective.
		 */
		*mmaped = mmap(map->mmaped, mmap_sz, prot,
				MAP_SHARED | MAP_FIXED, map_fd, 0);
		if (*mmaped == MAP_FAILED) {
			err = -errno;
			*mmaped = NULL;
			pr_warn("failed to re-mmap() map '%s': %d\n",
				 bpf_map__name(map), err);
			return err;
		}
	}

	return 0;
}

int bpf_object__attach_skeleton(struct bpf_object_skeleton *s)
{
	int i;

	for (i = 0; i < s->prog_cnt; i++) {
		struct bpf_program *prog = *s->progs[i].prog;
		struct bpf_link **link = s->progs[i].link;
		const struct bpf_sec_def *sec_def;
		const char *sec_name = bpf_program__title(prog, false);

		sec_def = find_sec_def(sec_name);
		if (!sec_def || !sec_def->attach_fn)
			continue;

		*link = sec_def->attach_fn(sec_def, prog);
		if (IS_ERR(*link)) {
			pr_warn("failed to auto-attach program '%s': %ld\n",
				bpf_program__name(prog), PTR_ERR(*link));
			return PTR_ERR(*link);
		}
	}

	return 0;
}

void bpf_object__detach_skeleton(struct bpf_object_skeleton *s)
{
	int i;

	for (i = 0; i < s->prog_cnt; i++) {
		struct bpf_link **link = s->progs[i].link;

		if (!IS_ERR_OR_NULL(*link))
			bpf_link__destroy(*link);
		*link = NULL;
	}
}

void bpf_object__destroy_skeleton(struct bpf_object_skeleton *s)
{
	if (s->progs)
		bpf_object__detach_skeleton(s);
	if (s->obj)
		bpf_object__close(*s->obj);
	free(s->maps);
	free(s->progs);
	free(s);
}
