/* Postprocess module symbol versions
 *
 * Copyright 2003       Kai Germaschewski
 * Copyright 2002-2004  Rusty Russell, IBM Corporation
 * Copyright 2006-2008  Sam Ravnborg
 * Based in part on module-init-tools/depmod.c,file2alias
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 *
 * Usage: modpost vmlinux module1.o module2.o ...
 */

#define _GNU_SOURCE
#include <elf.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <limits.h>
#include <stdbool.h>
#include <errno.h>
#include "modpost.h"
#include "../../include/linux/license.h"

/* Are we using CONFIG_MODVERSIONS? */
static bool modversions;
/* Is CONFIG_MODULE_SRCVERSION_ALL set? */
static bool all_versions;
/* If we are modposting external module set to 1 */
static bool external_module;
/* Only warn about unresolved symbols */
static bool warn_unresolved;

int sec_mismatch_count;
static bool sec_mismatch_warn_only = true;
/* ignore missing files */
static bool ignore_missing_files;
/* If set to 1, only warn (instead of error) about missing ns imports */
static bool allow_missing_ns_imports;

static bool error_occurred;

/*
 * Cut off the warnings when there are too many. This typically occurs when
 * vmlinux is missing. ('make modules' without building vmlinux.)
 */
#define MAX_UNRESOLVED_REPORTS	10
static unsigned int nr_unresolved;

/* In kernel, this size is defined in linux/module.h;
 * here we use Elf_Addr instead of long for covering cross-compile
 */

#define MODULE_NAME_LEN (64 - sizeof(Elf_Addr))

void __attribute__((format(printf, 2, 3)))
modpost_log(enum loglevel loglevel, const char *fmt, ...)
{
	va_list arglist;

	switch (loglevel) {
	case LOG_WARN:
		fprintf(stderr, "WARNING: ");
		break;
	case LOG_ERROR:
		fprintf(stderr, "ERROR: ");
		break;
	case LOG_FATAL:
		fprintf(stderr, "FATAL: ");
		break;
	default: /* invalid loglevel, ignore */
		break;
	}

	fprintf(stderr, "modpost: ");

	va_start(arglist, fmt);
	vfprintf(stderr, fmt, arglist);
	va_end(arglist);

	if (loglevel == LOG_FATAL)
		exit(1);
	if (loglevel == LOG_ERROR)
		error_occurred = true;
}

static inline bool strends(const char *str, const char *postfix)
{
	if (strlen(str) < strlen(postfix))
		return false;

	return strcmp(str + strlen(str) - strlen(postfix), postfix) == 0;
}

void *do_nofail(void *ptr, const char *expr)
{
	if (!ptr)
		fatal("Memory allocation failure: %s.\n", expr);

	return ptr;
}

char *read_text_file(const char *filename)
{
	struct stat st;
	size_t nbytes;
	int fd;
	char *buf;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		perror(filename);
		exit(1);
	}

	if (fstat(fd, &st) < 0) {
		perror(filename);
		exit(1);
	}

	buf = NOFAIL(malloc(st.st_size + 1));

	nbytes = st.st_size;

	while (nbytes) {
		ssize_t bytes_read;

		bytes_read = read(fd, buf, nbytes);
		if (bytes_read < 0) {
			perror(filename);
			exit(1);
		}

		nbytes -= bytes_read;
	}
	buf[st.st_size] = '\0';

	close(fd);

	return buf;
}

char *get_line(char **stringp)
{
	char *orig = *stringp, *next;

	/* do not return the unwanted extra line at EOF */
	if (!orig || *orig == '\0')
		return NULL;

	/* don't use strsep here, it is not available everywhere */
	next = strchr(orig, '\n');
	if (next)
		*next++ = '\0';

	*stringp = next;

	return orig;
}

/* A list of all modules we processed */
LIST_HEAD(modules);

static struct module *find_module(const char *modname)
{
	struct module *mod;

	list_for_each_entry(mod, &modules, list) {
		if (strcmp(mod->name, modname) == 0)
			return mod;
	}
	return NULL;
}

static struct module *new_module(const char *modname)
{
	struct module *mod;

	mod = NOFAIL(malloc(sizeof(*mod) + strlen(modname) + 1));
	memset(mod, 0, sizeof(*mod));

	INIT_LIST_HEAD(&mod->exported_symbols);
	INIT_LIST_HEAD(&mod->unresolved_symbols);
	INIT_LIST_HEAD(&mod->missing_namespaces);
	INIT_LIST_HEAD(&mod->imported_namespaces);

	strcpy(mod->name, modname);
	mod->is_vmlinux = (strcmp(modname, "vmlinux") == 0);

	/*
	 * Set mod->is_gpl_compatible to true by default. If MODULE_LICENSE()
	 * is missing, do not check the use for EXPORT_SYMBOL_GPL() becasue
	 * modpost will exit wiht error anyway.
	 */
	mod->is_gpl_compatible = true;

	list_add_tail(&mod->list, &modules);

	return mod;
}

/* A hash of all exported symbols,
 * struct symbol is also used for lists of unresolved symbols */

#define SYMBOL_HASH_SIZE 1024

struct symbol {
	struct symbol *next;
	struct list_head list;	/* link to module::exported_symbols or module::unresolved_symbols */
	struct module *module;
	char *namespace;
	unsigned int crc;
	bool crc_valid;
	bool weak;
	bool is_gpl_only;	/* exported by EXPORT_SYMBOL_GPL */
	char name[];
};

static struct symbol *symbolhash[SYMBOL_HASH_SIZE];

/* This is based on the hash algorithm from gdbm, via tdb */
static inline unsigned int tdb_hash(const char *name)
{
	unsigned value;	/* Used to compute the hash value.  */
	unsigned   i;	/* Used to cycle through random values. */

	/* Set the initial value from the key size. */
	for (value = 0x238F13AF * strlen(name), i = 0; name[i]; i++)
		value = (value + (((unsigned char *)name)[i] << (i*5 % 24)));

	return (1103515243 * value + 12345);
}

/**
 * Allocate a new symbols for use in the hash of exported symbols or
 * the list of unresolved symbols per module
 **/
static struct symbol *alloc_symbol(const char *name)
{
	struct symbol *s = NOFAIL(malloc(sizeof(*s) + strlen(name) + 1));

	memset(s, 0, sizeof(*s));
	strcpy(s->name, name);

	return s;
}

/* For the hash of exported symbols */
static void hash_add_symbol(struct symbol *sym)
{
	unsigned int hash;

	hash = tdb_hash(sym->name) % SYMBOL_HASH_SIZE;
	sym->next = symbolhash[hash];
	symbolhash[hash] = sym;
}

static void sym_add_unresolved(const char *name, struct module *mod, bool weak)
{
	struct symbol *sym;

	sym = alloc_symbol(name);
	sym->weak = weak;

	list_add_tail(&sym->list, &mod->unresolved_symbols);
}

static struct symbol *sym_find_with_module(const char *name, struct module *mod)
{
	struct symbol *s;

	/* For our purposes, .foo matches foo.  PPC64 needs this. */
	if (name[0] == '.')
		name++;

	for (s = symbolhash[tdb_hash(name) % SYMBOL_HASH_SIZE]; s; s = s->next) {
		if (strcmp(s->name, name) == 0 && (!mod || s->module == mod))
			return s;
	}
	return NULL;
}

static struct symbol *find_symbol(const char *name)
{
	return sym_find_with_module(name, NULL);
}

struct namespace_list {
	struct list_head list;
	char namespace[];
};

static bool contains_namespace(struct list_head *head, const char *namespace)
{
	struct namespace_list *list;

	list_for_each_entry(list, head, list) {
		if (!strcmp(list->namespace, namespace))
			return true;
	}

	return false;
}

static void add_namespace(struct list_head *head, const char *namespace)
{
	struct namespace_list *ns_entry;

	if (!contains_namespace(head, namespace)) {
		ns_entry = NOFAIL(malloc(sizeof(*ns_entry) +
					 strlen(namespace) + 1));
		strcpy(ns_entry->namespace, namespace);
		list_add_tail(&ns_entry->list, head);
	}
}

void *sym_get_data_by_offset(const struct elf_info *info,
			     unsigned int secindex, unsigned long offset)
{
	Elf_Shdr *sechdr = &info->sechdrs[secindex];

	if (info->hdr->e_type != ET_REL)
		offset -= sechdr->sh_addr;

	return (void *)info->hdr + sechdr->sh_offset + offset;
}

static void *sym_get_data(const struct elf_info *info, const Elf_Sym *sym)
{
	return sym_get_data_by_offset(info, get_secindex(info, sym),
				      sym->st_value);
}

const char *sech_name(const struct elf_info *info, Elf_Shdr *sechdr)
{
	return sym_get_data_by_offset(info, info->secindex_strings,
				      sechdr->sh_name);
}

const char *sec_name(const struct elf_info *info, int secindex)
{
	return sech_name(info, &info->sechdrs[secindex]);
}

static void sym_update_namespace(const char *symname, const char *namespace)
{
	struct symbol *s = find_symbol(symname);

	/*
	 * That symbol should have been created earlier and thus this is
	 * actually an assertion.
	 */
	if (!s) {
		error("Could not update namespace(%s) for symbol %s\n",
		      namespace, symname);
		return;
	}

	free(s->namespace);
	s->namespace = namespace[0] ? NOFAIL(strdup(namespace)) : NULL;
}

static struct symbol *sym_add_exported(const char *name, struct module *mod,
				       bool gpl_only)
{
	struct symbol *s = find_symbol(name);

	if (s && (!external_module || s->module->is_vmlinux || s->module == mod)) {
		error("%s: '%s' exported twice. Previous export was in %s%s\n",
		      mod->name, name, s->module->name,
		      s->module->is_vmlinux ? "" : ".ko");
	}

	s = alloc_symbol(name);
	s->module = mod;
	s->is_gpl_only = gpl_only;
	list_add_tail(&s->list, &mod->exported_symbols);
	hash_add_symbol(s);

	return s;
}

static void sym_set_crc(struct symbol *sym, unsigned int crc)
{
	sym->crc = crc;
	sym->crc_valid = true;
}

static void *grab_file(const char *filename, size_t *size)
{
	struct stat st;
	void *map = MAP_FAILED;
	int fd;

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &st))
		goto failed;

	*size = st.st_size;
	map = mmap(NULL, *size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);

failed:
	close(fd);
	if (map == MAP_FAILED)
		return NULL;
	return map;
}

static void release_file(void *file, size_t size)
{
	munmap(file, size);
}

static int parse_elf(struct elf_info *info, const char *filename)
{
	unsigned int i;
	Elf_Ehdr *hdr;
	Elf_Shdr *sechdrs;
	Elf_Sym  *sym;
	const char *secstrings;
	unsigned int symtab_idx = ~0U, symtab_shndx_idx = ~0U;

	hdr = grab_file(filename, &info->size);
	if (!hdr) {
		if (ignore_missing_files) {
			fprintf(stderr, "%s: %s (ignored)\n", filename,
				strerror(errno));
			return 0;
		}
		perror(filename);
		exit(1);
	}
	info->hdr = hdr;
	if (info->size < sizeof(*hdr)) {
		/* file too small, assume this is an empty .o file */
		return 0;
	}
	/* Is this a valid ELF file? */
	if ((hdr->e_ident[EI_MAG0] != ELFMAG0) ||
	    (hdr->e_ident[EI_MAG1] != ELFMAG1) ||
	    (hdr->e_ident[EI_MAG2] != ELFMAG2) ||
	    (hdr->e_ident[EI_MAG3] != ELFMAG3)) {
		/* Not an ELF file - silently ignore it */
		return 0;
	}
	/* Fix endianness in ELF header */
	hdr->e_type      = TO_NATIVE(hdr->e_type);
	hdr->e_machine   = TO_NATIVE(hdr->e_machine);
	hdr->e_version   = TO_NATIVE(hdr->e_version);
	hdr->e_entry     = TO_NATIVE(hdr->e_entry);
	hdr->e_phoff     = TO_NATIVE(hdr->e_phoff);
	hdr->e_shoff     = TO_NATIVE(hdr->e_shoff);
	hdr->e_flags     = TO_NATIVE(hdr->e_flags);
	hdr->e_ehsize    = TO_NATIVE(hdr->e_ehsize);
	hdr->e_phentsize = TO_NATIVE(hdr->e_phentsize);
	hdr->e_phnum     = TO_NATIVE(hdr->e_phnum);
	hdr->e_shentsize = TO_NATIVE(hdr->e_shentsize);
	hdr->e_shnum     = TO_NATIVE(hdr->e_shnum);
	hdr->e_shstrndx  = TO_NATIVE(hdr->e_shstrndx);
	sechdrs = (void *)hdr + hdr->e_shoff;
	info->sechdrs = sechdrs;

	/* Check if file offset is correct */
	if (hdr->e_shoff > info->size) {
		fatal("section header offset=%lu in file '%s' is bigger than filesize=%zu\n",
		      (unsigned long)hdr->e_shoff, filename, info->size);
		return 0;
	}

	if (hdr->e_shnum == SHN_UNDEF) {
		/*
		 * There are more than 64k sections,
		 * read count from .sh_size.
		 */
		info->num_sections = TO_NATIVE(sechdrs[0].sh_size);
	}
	else {
		info->num_sections = hdr->e_shnum;
	}
	if (hdr->e_shstrndx == SHN_XINDEX) {
		info->secindex_strings = TO_NATIVE(sechdrs[0].sh_link);
	}
	else {
		info->secindex_strings = hdr->e_shstrndx;
	}

	/* Fix endianness in section headers */
	for (i = 0; i < info->num_sections; i++) {
		sechdrs[i].sh_name      = TO_NATIVE(sechdrs[i].sh_name);
		sechdrs[i].sh_type      = TO_NATIVE(sechdrs[i].sh_type);
		sechdrs[i].sh_flags     = TO_NATIVE(sechdrs[i].sh_flags);
		sechdrs[i].sh_addr      = TO_NATIVE(sechdrs[i].sh_addr);
		sechdrs[i].sh_offset    = TO_NATIVE(sechdrs[i].sh_offset);
		sechdrs[i].sh_size      = TO_NATIVE(sechdrs[i].sh_size);
		sechdrs[i].sh_link      = TO_NATIVE(sechdrs[i].sh_link);
		sechdrs[i].sh_info      = TO_NATIVE(sechdrs[i].sh_info);
		sechdrs[i].sh_addralign = TO_NATIVE(sechdrs[i].sh_addralign);
		sechdrs[i].sh_entsize   = TO_NATIVE(sechdrs[i].sh_entsize);
	}
	/* Find symbol table. */
	secstrings = (void *)hdr + sechdrs[info->secindex_strings].sh_offset;
	for (i = 1; i < info->num_sections; i++) {
		const char *secname;
		int nobits = sechdrs[i].sh_type == SHT_NOBITS;

		if (!nobits && sechdrs[i].sh_offset > info->size) {
			fatal("%s is truncated. sechdrs[i].sh_offset=%lu > "
			      "sizeof(*hrd)=%zu\n", filename,
			      (unsigned long)sechdrs[i].sh_offset,
			      sizeof(*hdr));
			return 0;
		}
		secname = secstrings + sechdrs[i].sh_name;
		if (strcmp(secname, ".modinfo") == 0) {
			if (nobits)
				fatal("%s has NOBITS .modinfo\n", filename);
			info->modinfo = (void *)hdr + sechdrs[i].sh_offset;
			info->modinfo_len = sechdrs[i].sh_size;
		}

		if (sechdrs[i].sh_type == SHT_SYMTAB) {
			unsigned int sh_link_idx;
			symtab_idx = i;
			info->symtab_start = (void *)hdr +
			    sechdrs[i].sh_offset;
			info->symtab_stop  = (void *)hdr +
			    sechdrs[i].sh_offset + sechdrs[i].sh_size;
			sh_link_idx = sechdrs[i].sh_link;
			info->strtab       = (void *)hdr +
			    sechdrs[sh_link_idx].sh_offset;
		}

		/* 32bit section no. table? ("more than 64k sections") */
		if (sechdrs[i].sh_type == SHT_SYMTAB_SHNDX) {
			symtab_shndx_idx = i;
			info->symtab_shndx_start = (void *)hdr +
			    sechdrs[i].sh_offset;
			info->symtab_shndx_stop  = (void *)hdr +
			    sechdrs[i].sh_offset + sechdrs[i].sh_size;
		}
	}
	if (!info->symtab_start)
		fatal("%s has no symtab?\n", filename);

	/* Fix endianness in symbols */
	for (sym = info->symtab_start; sym < info->symtab_stop; sym++) {
		sym->st_shndx = TO_NATIVE(sym->st_shndx);
		sym->st_name  = TO_NATIVE(sym->st_name);
		sym->st_value = TO_NATIVE(sym->st_value);
		sym->st_size  = TO_NATIVE(sym->st_size);
	}

	if (symtab_shndx_idx != ~0U) {
		Elf32_Word *p;
		if (symtab_idx != sechdrs[symtab_shndx_idx].sh_link)
			fatal("%s: SYMTAB_SHNDX has bad sh_link: %u!=%u\n",
			      filename, sechdrs[symtab_shndx_idx].sh_link,
			      symtab_idx);
		/* Fix endianness */
		for (p = info->symtab_shndx_start; p < info->symtab_shndx_stop;
		     p++)
			*p = TO_NATIVE(*p);
	}

	return 1;
}

static void parse_elf_finish(struct elf_info *info)
{
	release_file(info->hdr, info->size);
}

static int ignore_undef_symbol(struct elf_info *info, const char *symname)
{
	/* ignore __this_module, it will be resolved shortly */
	if (strcmp(symname, "__this_module") == 0)
		return 1;
	/* ignore global offset table */
	if (strcmp(symname, "_GLOBAL_OFFSET_TABLE_") == 0)
		return 1;
	if (info->hdr->e_machine == EM_PPC)
		/* Special register function linked on all modules during final link of .ko */
		if (strstarts(symname, "_restgpr_") ||
		    strstarts(symname, "_savegpr_") ||
		    strstarts(symname, "_rest32gpr_") ||
		    strstarts(symname, "_save32gpr_") ||
		    strstarts(symname, "_restvr_") ||
		    strstarts(symname, "_savevr_"))
			return 1;
	if (info->hdr->e_machine == EM_PPC64)
		/* Special register function linked on all modules during final link of .ko */
		if (strstarts(symname, "_restgpr0_") ||
		    strstarts(symname, "_savegpr0_") ||
		    strstarts(symname, "_restvr_") ||
		    strstarts(symname, "_savevr_") ||
		    strcmp(symname, ".TOC.") == 0)
			return 1;

	if (info->hdr->e_machine == EM_S390)
		/* Expoline thunks are linked on all kernel modules during final link of .ko */
		if (strstarts(symname, "__s390_indirect_jump_r"))
			return 1;
	/* Do not ignore this symbol */
	return 0;
}

static void handle_symbol(struct module *mod, struct elf_info *info,
			  const Elf_Sym *sym, const char *symname)
{
	switch (sym->st_shndx) {
	case SHN_COMMON:
		if (strstarts(symname, "__gnu_lto_")) {
			/* Should warn here, but modpost runs before the linker */
		} else
			warn("\"%s\" [%s] is COMMON symbol\n", symname, mod->name);
		break;
	case SHN_UNDEF:
		/* undefined symbol */
		if (ELF_ST_BIND(sym->st_info) != STB_GLOBAL &&
		    ELF_ST_BIND(sym->st_info) != STB_WEAK)
			break;
		if (ignore_undef_symbol(info, symname))
			break;
		if (info->hdr->e_machine == EM_SPARC ||
		    info->hdr->e_machine == EM_SPARCV9) {
			/* Ignore register directives. */
			if (ELF_ST_TYPE(sym->st_info) == STT_SPARC_REGISTER)
				break;
			if (symname[0] == '.') {
				char *munged = NOFAIL(strdup(symname));
				munged[0] = '_';
				munged[1] = toupper(munged[1]);
				symname = munged;
			}
		}

		sym_add_unresolved(symname, mod,
				   ELF_ST_BIND(sym->st_info) == STB_WEAK);
		break;
	default:
		/* All exported symbols */
		if (strstarts(symname, "__ksymtab_")) {
			const char *name, *secname;

			name = symname + strlen("__ksymtab_");
			secname = sec_name(info, get_secindex(info, sym));

			if (strstarts(secname, "___ksymtab_gpl+"))
				sym_add_exported(name, mod, true);
			else if (strstarts(secname, "___ksymtab+"))
				sym_add_exported(name, mod, false);
		}
		if (strcmp(symname, "init_module") == 0)
			mod->has_init = true;
		if (strcmp(symname, "cleanup_module") == 0)
			mod->has_cleanup = true;
		break;
	}
}

/**
 * Parse tag=value strings from .modinfo section
 **/
static char *next_string(char *string, unsigned long *secsize)
{
	/* Skip non-zero chars */
	while (string[0]) {
		string++;
		if ((*secsize)-- <= 1)
			return NULL;
	}

	/* Skip any zero padding. */
	while (!string[0]) {
		string++;
		if ((*secsize)-- <= 1)
			return NULL;
	}
	return string;
}

static char *get_next_modinfo(struct elf_info *info, const char *tag,
			      char *prev)
{
	char *p;
	unsigned int taglen = strlen(tag);
	char *modinfo = info->modinfo;
	unsigned long size = info->modinfo_len;

	if (prev) {
		size -= prev - modinfo;
		modinfo = next_string(prev, &size);
	}

	for (p = modinfo; p; p = next_string(p, &size)) {
		if (strncmp(p, tag, taglen) == 0 && p[taglen] == '=')
			return p + taglen + 1;
	}
	return NULL;
}

static char *get_modinfo(struct elf_info *info, const char *tag)

{
	return get_next_modinfo(info, tag, NULL);
}

static char *remove_dot(char *s)
{
	size_t n = strcspn(s, ".");

	if (n && s[n]) {
		size_t m = strspn(s + n + 1, "0123456789");
		if (m && (s[n + m] == '.' || s[n + m] == 0))
			s[n] = 0;
	}
	return s;
}

/*
 * The CRCs are recorded in .*.cmd files in the form of:
 * #SYMVER <name> <crc>
 */
static void extract_crcs_for_object(const char *object, struct module *mod)
{
	char cmd_file[PATH_MAX];
	char *buf, *p;
	const char *base;
	int dirlen, ret;

	base = strrchr(object, '/');
	if (base) {
		base++;
		dirlen = base - object;
	} else {
		dirlen = 0;
		base = object;
	}

	ret = snprintf(cmd_file, sizeof(cmd_file), "%.*s.%s.cmd",
		       dirlen, object, base);
	if (ret >= sizeof(cmd_file)) {
		error("%s: too long path was truncated\n", cmd_file);
		return;
	}

	buf = read_text_file(cmd_file);
	p = buf;

	while ((p = strstr(p, "\n#SYMVER "))) {
		char *name;
		size_t namelen;
		unsigned int crc;
		struct symbol *sym;

		name = p + strlen("\n#SYMVER ");

		p = strchr(name, ' ');
		if (!p)
			break;

		namelen = p - name;
		p++;

		if (!isdigit(*p))
			continue;	/* skip this line */

		crc = strtol(p, &p, 0);
		if (*p != '\n')
			continue;	/* skip this line */

		name[namelen] = '\0';

		/*
		 * sym_find_with_module() may return NULL here.
		 * It typically occurs when CONFIG_TRIM_UNUSED_KSYMS=y.
		 * Since commit e1327a127703, genksyms calculates CRCs of all
		 * symbols, including trimmed ones. Ignore orphan CRCs.
		 */
		sym = sym_find_with_module(name, mod);
		if (sym)
			sym_set_crc(sym, crc);
	}

	free(buf);
}

/*
 * The symbol versions (CRC) are recorded in the .*.cmd files.
 * Parse them to retrieve CRCs for the current module.
 */
static void mod_set_crcs(struct module *mod)
{
	char objlist[PATH_MAX];
	char *buf, *p, *obj;
	int ret;

	if (mod->is_vmlinux) {
		strcpy(objlist, ".vmlinux.objs");
	} else {
		/* objects for a module are listed in the *.mod file. */
		ret = snprintf(objlist, sizeof(objlist), "%s.mod", mod->name);
		if (ret >= sizeof(objlist)) {
			error("%s: too long path was truncated\n", objlist);
			return;
		}
	}

	buf = read_text_file(objlist);
	p = buf;

	while ((obj = strsep(&p, "\n")) && obj[0])
		extract_crcs_for_object(obj, mod);

	free(buf);
}

static void read_symbols(const char *modname)
{
	const char *symname;
	char *version;
	char *license;
	char *namespace;
	struct module *mod;
	struct elf_info info = { };
	Elf_Sym *sym;

	if (!parse_elf(&info, modname))
		return;

	{
		char *tmp;

		/* strip trailing .o */
		tmp = NOFAIL(strdup(modname));
		tmp[strlen(tmp) - 2] = '\0';
		mod = new_module(tmp);
		free(tmp);
	}

	if (!mod->is_vmlinux) {
		license = get_modinfo(&info, "license");
		if (!license)
			error("missing MODULE_LICENSE() in %s\n", modname);
		while (license) {
			if (!license_is_gpl_compatible(license)) {
				mod->is_gpl_compatible = false;
				break;
			}
			license = get_next_modinfo(&info, "license", license);
		}

		namespace = get_modinfo(&info, "import_ns");
		while (namespace) {
			add_namespace(&mod->imported_namespaces, namespace);
			namespace = get_next_modinfo(&info, "import_ns",
						     namespace);
		}
	}

	for (sym = info.symtab_start; sym < info.symtab_stop; sym++) {
		symname = remove_dot(info.strtab + sym->st_name);

		handle_symbol(mod, &info, sym, symname);
		handle_moddevtable(mod, &info, sym, symname);
	}

	for (sym = info.symtab_start; sym < info.symtab_stop; sym++) {
		symname = remove_dot(info.strtab + sym->st_name);

		/* Apply symbol namespaces from __kstrtabns_<symbol> entries. */
		if (strstarts(symname, "__kstrtabns_"))
			sym_update_namespace(symname + strlen("__kstrtabns_"),
					     sym_get_data(&info, sym));
	}

	check_sec_ref(mod, modname, &info);

	if (!mod->is_vmlinux) {
		version = get_modinfo(&info, "version");
		if (version || all_versions)
			get_src_version(mod->name, mod->srcversion,
					sizeof(mod->srcversion) - 1);
	}

	parse_elf_finish(&info);

	if (modversions) {
		/*
		 * Our trick to get versioning for module struct etc. - it's
		 * never passed as an argument to an exported function, so
		 * the automatic versioning doesn't pick it up, but it's really
		 * important anyhow.
		 */
		sym_add_unresolved("module_layout", mod, false);

		mod_set_crcs(mod);
	}
}

static void read_symbols_from_files(const char *filename)
{
	FILE *in = stdin;
	char fname[PATH_MAX];

	if (strcmp(filename, "-") != 0) {
		in = fopen(filename, "r");
		if (!in)
			fatal("Can't open filenames file %s: %m", filename);
	}

	while (fgets(fname, PATH_MAX, in) != NULL) {
		if (strends(fname, "\n"))
			fname[strlen(fname)-1] = '\0';
		read_symbols(fname);
	}

	if (in != stdin)
		fclose(in);
}

#define SZ 500

/* We first write the generated file into memory using the
 * following helper, then compare to the file on disk and
 * only update the later if anything changed */

void __attribute__((format(printf, 2, 3))) buf_printf(struct buffer *buf,
						      const char *fmt, ...)
{
	char tmp[SZ];
	int len;
	va_list ap;

	va_start(ap, fmt);
	len = vsnprintf(tmp, SZ, fmt, ap);
	buf_write(buf, tmp, len);
	va_end(ap);
}

void buf_write(struct buffer *buf, const char *s, int len)
{
	if (buf->size - buf->pos < len) {
		buf->size += len + SZ;
		buf->p = NOFAIL(realloc(buf->p, buf->size));
	}
	strncpy(buf->p + buf->pos, s, len);
	buf->pos += len;
}

static void check_exports(struct module *mod)
{
	struct symbol *s, *exp;

	list_for_each_entry(s, &mod->unresolved_symbols, list) {
		const char *basename;
		exp = find_symbol(s->name);
		if (!exp) {
			if (!s->weak && nr_unresolved++ < MAX_UNRESOLVED_REPORTS)
				modpost_log(warn_unresolved ? LOG_WARN : LOG_ERROR,
					    "\"%s\" [%s.ko] undefined!\n",
					    s->name, mod->name);
			continue;
		}
		if (exp->module == mod) {
			error("\"%s\" [%s.ko] was exported without definition\n",
			      s->name, mod->name);
			continue;
		}

		s->module = exp->module;
		s->crc_valid = exp->crc_valid;
		s->crc = exp->crc;

		basename = strrchr(mod->name, '/');
		if (basename)
			basename++;
		else
			basename = mod->name;

		if (exp->namespace &&
		    !contains_namespace(&mod->imported_namespaces, exp->namespace)) {
			modpost_log(allow_missing_ns_imports ? LOG_WARN : LOG_ERROR,
				    "module %s uses symbol %s from namespace %s, but does not import it.\n",
				    basename, exp->name, exp->namespace);
			add_namespace(&mod->missing_namespaces, exp->namespace);
		}

		if (!mod->is_gpl_compatible && exp->is_gpl_only)
			error("GPL-incompatible module %s.ko uses GPL-only symbol '%s'\n",
			      basename, exp->name);
	}
}

static void check_modname_len(struct module *mod)
{
	const char *mod_name;

	mod_name = strrchr(mod->name, '/');
	if (mod_name == NULL)
		mod_name = mod->name;
	else
		mod_name++;
	if (strlen(mod_name) >= MODULE_NAME_LEN)
		error("module name is too long [%s.ko]\n", mod->name);
}

/**
 * Header for the generated file
 **/
static void add_header(struct buffer *b, struct module *mod)
{
	buf_printf(b, "#include <linux/module.h>\n");
	/*
	 * Include build-salt.h after module.h in order to
	 * inherit the definitions.
	 */
	buf_printf(b, "#define INCLUDE_VERMAGIC\n");
	buf_printf(b, "#include <linux/build-salt.h>\n");
	buf_printf(b, "#include <linux/elfnote-lto.h>\n");
	buf_printf(b, "#include <linux/export-internal.h>\n");
	buf_printf(b, "#include <linux/vermagic.h>\n");
	buf_printf(b, "#include <linux/compiler.h>\n");
	buf_printf(b, "\n");
	buf_printf(b, "BUILD_SALT;\n");
	buf_printf(b, "BUILD_LTO_INFO;\n");
	buf_printf(b, "\n");
	buf_printf(b, "MODULE_INFO(vermagic, VERMAGIC_STRING);\n");
	buf_printf(b, "MODULE_INFO(name, KBUILD_MODNAME);\n");
	buf_printf(b, "\n");
	buf_printf(b, "__visible struct module __this_module\n");
	buf_printf(b, "__section(\".gnu.linkonce.this_module\") = {\n");
	buf_printf(b, "\t.name = KBUILD_MODNAME,\n");
	if (mod->has_init)
		buf_printf(b, "\t.init = init_module,\n");
	if (mod->has_cleanup)
		buf_printf(b, "#ifdef CONFIG_MODULE_UNLOAD\n"
			      "\t.exit = cleanup_module,\n"
			      "#endif\n");
	buf_printf(b, "\t.arch = MODULE_ARCH_INIT,\n");
	buf_printf(b, "};\n");

	if (!external_module)
		buf_printf(b, "\nMODULE_INFO(intree, \"Y\");\n");

	buf_printf(b,
		   "\n"
		   "#ifdef CONFIG_RETPOLINE\n"
		   "MODULE_INFO(retpoline, \"Y\");\n"
		   "#endif\n");

	if (strstarts(mod->name, "drivers/staging"))
		buf_printf(b, "\nMODULE_INFO(staging, \"Y\");\n");
}

static void add_exported_symbols(struct buffer *buf, struct module *mod)
{
	struct symbol *sym;

	if (!modversions)
		return;

	/* record CRCs for exported symbols */
	buf_printf(buf, "\n");
	list_for_each_entry(sym, &mod->exported_symbols, list) {
		if (!sym->crc_valid) {
			warn("EXPORT symbol \"%s\" [%s%s] version generation failed, symbol will not be versioned.\n"
			     "Is \"%s\" prototyped in <asm/asm-prototypes.h>?\n",
			     sym->name, mod->name, mod->is_vmlinux ? "" : ".ko",
			     sym->name);
			continue;
		}

		buf_printf(buf, "SYMBOL_CRC(%s, 0x%08x, \"%s\");\n",
			   sym->name, sym->crc, sym->is_gpl_only ? "_gpl" : "");
	}
}

/**
 * Record CRCs for unresolved symbols
 **/
static void add_versions(struct buffer *b, struct module *mod)
{
	struct symbol *s;

	if (!modversions)
		return;

	buf_printf(b, "\n");
	buf_printf(b, "static const struct modversion_info ____versions[]\n");
	buf_printf(b, "__used __section(\"__versions\") = {\n");

	list_for_each_entry(s, &mod->unresolved_symbols, list) {
		if (!s->module)
			continue;
		if (!s->crc_valid) {
			warn("\"%s\" [%s.ko] has no CRC!\n",
				s->name, mod->name);
			continue;
		}
		if (strlen(s->name) >= MODULE_NAME_LEN) {
			error("too long symbol \"%s\" [%s.ko]\n",
			      s->name, mod->name);
			break;
		}
		buf_printf(b, "\t{ %#8x, \"%s\" },\n",
			   s->crc, s->name);
	}

	buf_printf(b, "};\n");
}

static void add_depends(struct buffer *b, struct module *mod)
{
	struct symbol *s;
	int first = 1;

	/* Clear ->seen flag of modules that own symbols needed by this. */
	list_for_each_entry(s, &mod->unresolved_symbols, list) {
		if (s->module)
			s->module->seen = s->module->is_vmlinux;
	}

	buf_printf(b, "\n");
	buf_printf(b, "MODULE_INFO(depends, \"");
	list_for_each_entry(s, &mod->unresolved_symbols, list) {
		const char *p;
		if (!s->module)
			continue;

		if (s->module->seen)
			continue;

		s->module->seen = true;
		p = strrchr(s->module->name, '/');
		if (p)
			p++;
		else
			p = s->module->name;
		buf_printf(b, "%s%s", first ? "" : ",", p);
		first = 0;
	}
	buf_printf(b, "\");\n");
}

static void add_srcversion(struct buffer *b, struct module *mod)
{
	if (mod->srcversion[0]) {
		buf_printf(b, "\n");
		buf_printf(b, "MODULE_INFO(srcversion, \"%s\");\n",
			   mod->srcversion);
	}
}

static void write_buf(struct buffer *b, const char *fname)
{
	FILE *file;

	if (error_occurred)
		return;

	file = fopen(fname, "w");
	if (!file) {
		perror(fname);
		exit(1);
	}
	if (fwrite(b->p, 1, b->pos, file) != b->pos) {
		perror(fname);
		exit(1);
	}
	if (fclose(file) != 0) {
		perror(fname);
		exit(1);
	}
}

static void write_if_changed(struct buffer *b, const char *fname)
{
	char *tmp;
	FILE *file;
	struct stat st;

	file = fopen(fname, "r");
	if (!file)
		goto write;

	if (fstat(fileno(file), &st) < 0)
		goto close_write;

	if (st.st_size != b->pos)
		goto close_write;

	tmp = NOFAIL(malloc(b->pos));
	if (fread(tmp, 1, b->pos, file) != b->pos)
		goto free_write;

	if (memcmp(tmp, b->p, b->pos) != 0)
		goto free_write;

	free(tmp);
	fclose(file);
	return;

 free_write:
	free(tmp);
 close_write:
	fclose(file);
 write:
	write_buf(b, fname);
}

static void write_vmlinux_export_c_file(struct module *mod)
{
	struct buffer buf = { };

	buf_printf(&buf,
		   "#include <linux/export-internal.h>\n");

	add_exported_symbols(&buf, mod);
	write_if_changed(&buf, ".vmlinux.export.c");
	free(buf.p);
}

/* do sanity checks, and generate *.mod.c file */
static void write_mod_c_file(struct module *mod)
{
	struct buffer buf = { };
	char fname[PATH_MAX];
	int ret;

	check_modname_len(mod);
	check_exports(mod);

	add_header(&buf, mod);
	add_exported_symbols(&buf, mod);
	add_versions(&buf, mod);
	add_depends(&buf, mod);
	add_moddevtable(&buf, mod);
	add_srcversion(&buf, mod);

	ret = snprintf(fname, sizeof(fname), "%s.mod.c", mod->name);
	if (ret >= sizeof(fname)) {
		error("%s: too long path was truncated\n", fname);
		goto free;
	}

	write_if_changed(&buf, fname);

free:
	free(buf.p);
}

/* parse Module.symvers file. line format:
 * 0x12345678<tab>symbol<tab>module<tab>export<tab>namespace
 **/
static void read_dump(const char *fname)
{
	char *buf, *pos, *line;

	buf = read_text_file(fname);
	if (!buf)
		/* No symbol versions, silently ignore */
		return;

	pos = buf;

	while ((line = get_line(&pos))) {
		char *symname, *namespace, *modname, *d, *export;
		unsigned int crc;
		struct module *mod;
		struct symbol *s;
		bool gpl_only;

		if (!(symname = strchr(line, '\t')))
			goto fail;
		*symname++ = '\0';
		if (!(modname = strchr(symname, '\t')))
			goto fail;
		*modname++ = '\0';
		if (!(export = strchr(modname, '\t')))
			goto fail;
		*export++ = '\0';
		if (!(namespace = strchr(export, '\t')))
			goto fail;
		*namespace++ = '\0';

		crc = strtoul(line, &d, 16);
		if (*symname == '\0' || *modname == '\0' || *d != '\0')
			goto fail;

		if (!strcmp(export, "EXPORT_SYMBOL_GPL")) {
			gpl_only = true;
		} else if (!strcmp(export, "EXPORT_SYMBOL")) {
			gpl_only = false;
		} else {
			error("%s: unknown license %s. skip", symname, export);
			continue;
		}

		mod = find_module(modname);
		if (!mod) {
			mod = new_module(modname);
			mod->from_dump = true;
		}
		s = sym_add_exported(symname, mod, gpl_only);
		sym_set_crc(s, crc);
		sym_update_namespace(symname, namespace);
	}
	free(buf);
	return;
fail:
	free(buf);
	fatal("parse error in symbol dump file\n");
}

static void write_dump(const char *fname)
{
	struct buffer buf = { };
	struct module *mod;
	struct symbol *sym;

	list_for_each_entry(mod, &modules, list) {
		if (mod->from_dump)
			continue;
		list_for_each_entry(sym, &mod->exported_symbols, list) {
			buf_printf(&buf, "0x%08x\t%s\t%s\tEXPORT_SYMBOL%s\t%s\n",
				   sym->crc, sym->name, mod->name,
				   sym->is_gpl_only ? "_GPL" : "",
				   sym->namespace ?: "");
		}
	}
	write_buf(&buf, fname);
	free(buf.p);
}

static void write_namespace_deps_files(const char *fname)
{
	struct module *mod;
	struct namespace_list *ns;
	struct buffer ns_deps_buf = {};

	list_for_each_entry(mod, &modules, list) {

		if (mod->from_dump || list_empty(&mod->missing_namespaces))
			continue;

		buf_printf(&ns_deps_buf, "%s.ko:", mod->name);

		list_for_each_entry(ns, &mod->missing_namespaces, list)
			buf_printf(&ns_deps_buf, " %s", ns->namespace);

		buf_printf(&ns_deps_buf, "\n");
	}

	write_if_changed(&ns_deps_buf, fname);
	free(ns_deps_buf.p);
}

struct dump_list {
	struct list_head list;
	const char *file;
};

int main(int argc, char **argv)
{
	struct module *mod;
	char *missing_namespace_deps = NULL;
	char *dump_write = NULL, *files_source = NULL;
	int opt;
	LIST_HEAD(dump_lists);
	struct dump_list *dl, *dl2;

	while ((opt = getopt(argc, argv, "ei:mnT:o:awENd:")) != -1) {
		switch (opt) {
		case 'e':
			external_module = true;
			break;
		case 'i':
			dl = NOFAIL(malloc(sizeof(*dl)));
			dl->file = optarg;
			list_add_tail(&dl->list, &dump_lists);
			break;
		case 'm':
			modversions = true;
			break;
		case 'n':
			ignore_missing_files = true;
			break;
		case 'o':
			dump_write = optarg;
			break;
		case 'a':
			all_versions = true;
			break;
		case 'T':
			files_source = optarg;
			break;
		case 'w':
			warn_unresolved = true;
			break;
		case 'E':
			sec_mismatch_warn_only = false;
			break;
		case 'N':
			allow_missing_ns_imports = true;
			break;
		case 'd':
			missing_namespace_deps = optarg;
			break;
		default:
			exit(1);
		}
	}

	list_for_each_entry_safe(dl, dl2, &dump_lists, list) {
		read_dump(dl->file);
		list_del(&dl->list);
		free(dl);
	}

	while (optind < argc)
		read_symbols(argv[optind++]);

	if (files_source)
		read_symbols_from_files(files_source);

	list_for_each_entry(mod, &modules, list) {
		if (mod->from_dump)
			continue;

		if (mod->is_vmlinux)
			write_vmlinux_export_c_file(mod);
		else
			write_mod_c_file(mod);
	}

	if (missing_namespace_deps)
		write_namespace_deps_files(missing_namespace_deps);

	if (dump_write)
		write_dump(dump_write);
	if (sec_mismatch_count && !sec_mismatch_warn_only)
		error("Section mismatches detected.\n"
		      "Set CONFIG_SECTION_MISMATCH_WARN_ONLY=y to allow them.\n");

	if (nr_unresolved > MAX_UNRESOLVED_REPORTS)
		warn("suppressed %u unresolved symbol warnings because there were too many)\n",
		     nr_unresolved - MAX_UNRESOLVED_REPORTS);

	return error_occurred ? 1 : 0;
}
