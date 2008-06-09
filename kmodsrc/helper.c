/*  Copyright (C) 2008  Jeffrey Brian Arnold <jbarnold@mit.edu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License, version 2.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston, MA
 *  02110-1301, USA.
 */

#include "modcommon.h"
#include "helper.h"
#include "jumps.h"
#include "nops.h"
#include <linux/kthread.h>

/* defined by modcommon.c */
extern int debug;

/* defined by ksplice-create */
extern struct ksplice_size ksplice_sizes;

#undef max
#define max(a, b) ((a) > (b) ? (a) : (b))

int
init_module(void)
{
	printk("ksplice_h: Preparing and checking %s\n", ksplice_name);

	if (ksplice_do_helper() != 0)
		return -1;

	if (ksplice_do_primary() != 0)
		return -1;

	return 0;
}

void
cleanup_module(void)
{
	/* These pointers should always be NULL when no helper module is loaded */
	release_list((struct starts_with_next *) reloc_namevals);
	release_list((struct starts_with_next *) reloc_addrmaps);
	release_list((struct starts_with_next *) safety_records);
}

int
ksplice_do_helper(void)
{
	struct ksplice_size *s;
	int i, record_count = 0, ret;
	char *finished;
	int numfinished, oldfinished = 0;
	int restart_count = 0, stage = 1;

	if (process_ksplice_relocs(1) != 0)
		return -1;

	for (s = &ksplice_sizes; s->name != NULL; s++) {
		record_count++;
	}

	/* old kernels do not have kcalloc */
	finished = ksplice_kcalloc(record_count);

      start:
	for (s = &ksplice_sizes, i = 0; s->name != NULL; s++, i++) {
		if (s->size == 0)
			finished[i] = 1;
		if (finished[i])
			continue;

		ret = search_for_match(s, &stage);
		if (ret < 0) {
			kfree(finished);
			return ret;
		} else if (ret == 0) {
			finished[i] = 1;
		}
	}

	numfinished = 0;
	for (i = 0; i < record_count; i++) {
		if (finished[i])
			numfinished++;
	}
	if (numfinished == record_count) {
		kfree(finished);
		return 0;
	}

	if (oldfinished == numfinished) {
		if (stage < 3) {
			stage++;
			goto start;
		}
		print_abort("run-pre: could not match some sections");
		kfree(finished);
		return -1;
	}
	oldfinished = numfinished;

	if (restart_count < 20) {
		restart_count++;
		goto start;
	}
	print_abort("run-pre: restart limit exceeded");
	kfree(finished);
	return -1;
}

/* old kernels do not have kcalloc */
void *
ksplice_kcalloc(int size)
{
	char *mem = kmalloc(size, GFP_KERNEL);
	int i;
	for (i = 0; i < size; i++) {
		mem[i] = 0;
	}
	return mem;
}

int
search_for_match(struct ksplice_size *s, int *stage)
{
	int i, saved_debug;
	long run_addr;
	struct ansglob *glob = NULL, *g;

	for (i = 0; i < s->num_sym_addrs; i++) {
		add2glob(&glob, s->sym_addrs[i]);
	}

	compute_address(s->name, &glob);
	if (*stage <= 1 && !singular(glob)) {
		release(&glob);
		return 1;
	}

	if (debug >= 3) {
		printk("ksplice_h: run-pre: starting sect search for %s\n",
		       s->name);
	}

	for (g = glob; g != NULL; g = g->next) {
		run_addr = g->val;

		yield();
		if (try_addr(s, run_addr, s->thismod_addr, !singular(glob))) {
			release(&glob);
			return 0;
		}
	}
	release(&glob);

	if (*stage <= 2)
		return 1;

	saved_debug = debug;
	debug = 0;
	brute_search_all_mods(s);
	debug = saved_debug;
	return 1;
}

int
try_addr(struct ksplice_size *s, long run_addr, long pre_addr,
	 int create_nameval)
{
	struct safety_record *tmp;

	if (run_pre_cmp(run_addr, pre_addr, s->size, 0) != 0) {
		set_temp_myst_relocs(NOVAL);
		if (debug >= 1) {
			printk("ksplice_h: run-pre: sect %s does not match ",
			       s->name);
			printk("(r_a=%08lx p_a=%08lx s=%ld)\n",
			       run_addr, pre_addr, s->size);
			printk("ksplice_h: run-pre: ");
			run_pre_cmp(run_addr, pre_addr, s->size, 1);
			printk("\n");
		}
	} else {
		set_temp_myst_relocs(VAL);

		if (debug >= 3) {
			printk("ksplice_h: run-pre: found sect %s=%08lx\n",
			       s->name, run_addr);
		}

		tmp = kmalloc(sizeof (*tmp), GFP_KERNEL);
		tmp->addr = run_addr;
		tmp->size = s->size;
		tmp->next = safety_records;
		tmp->care = 0;
		safety_records = tmp;

		if (create_nameval) {
			struct reloc_nameval *nv = find_nameval(s->name, 1);
			nv->val = run_addr;
			nv->status = VAL;
		}

		return 1;
	}
	return 0;
}

int
run_pre_cmp(long run_addr, long pre_addr, int size, int rerun)
{
	int run_o, pre_o, lenient = 0, prev_c3 = 0, recent_5b = 0;
	unsigned char run, pre;
	struct reloc_addrmap *map;

	if (size == 0)
		return 1;

	for (run_o = 0, pre_o = 0; run_o < size && pre_o < size;
	     pre_o++, run_o++) {
		if (lenient > 0)
			lenient--;
		if (prev_c3 > 0)
			prev_c3--;
		if (recent_5b > 0)
			recent_5b--;

		if (!virtual_address_mapped(run_addr + run_o))
			return 1;
		run = *(unsigned char *) (run_addr + run_o);
		pre = *(unsigned char *) (pre_addr + pre_o);

		if (rerun)
			printk("%02x/%02x ", run, pre);

		if (run == pre) {
			if ((map = find_addrmap(pre_addr + pre_o)) != NULL) {
				if (handle_myst_reloc
				    (pre_addr, &pre_o, run_addr, &run_o,
				     map, rerun) == 1)
					return 1;
				continue;
			}
			if (pre == 0xc3)
				prev_c3 = 1 + 1;
			if (pre == 0x5b)
				recent_5b = 10 + 1;
			if (jumplen[pre])
				lenient = max(jumplen[pre] + 1, lenient);
			if (match_nop(run_addr, &run_o, &pre_o) ||
			    match_nop(pre_addr, &pre_o, &run_o))
				continue;
			continue;
		}

		if ((map = find_addrmap(pre_addr + pre_o)) != NULL) {
			if (handle_myst_reloc
			    (pre_addr, &pre_o, run_addr, &run_o, map,
			     rerun) == 1)
				return 1;
			continue;
		}
		if (prev_c3 && recent_5b)
			return 0;
		if (match_nop(run_addr, &run_o, &pre_o) ||
		    match_nop(pre_addr, &pre_o, &run_o))
			continue;
		if (jumplen[run] && jumplen[pre]) {
			run_o += jumplen[run];
			pre_o += jumplen[pre];
			continue;
		}
		if (lenient)
			continue;
		if (rerun) {
			printk("[p_o=%08x] ! %02x/%02x %02x/%02x",
			       pre_o,
			       *(unsigned char *) (run_addr + run_o + 1),
			       *(unsigned char *) (pre_addr + pre_o + 1),
			       *(unsigned char *) (run_addr + run_o + 2),
			       *(unsigned char *) (pre_addr + pre_o + 2));
		}
		return 1;
	}
	return 0;
}

int
handle_myst_reloc(long pre_addr, int *pre_o, long run_addr,
		  int *run_o, struct reloc_addrmap *map, int rerun)
{
	int expected;
	int offset = (int) (pre_addr + *pre_o - map->addr);
	int run_reloc = *(int *) (run_addr + *run_o - offset);

	if (debug >= 3 && !rerun) {
		printk("ksplice_h: run-pre: reloc at r_a=%08lx p_o=%08x: ",
		       run_addr, *pre_o);
		printk("%s=%08lx (A=%08lx *r=%08x)\n",
		       map->nameval->name, map->nameval->val,
		       map->addend, run_reloc);
	}

	if (!starts_with(map->nameval->name, ".rodata.str")) {
		expected = run_reloc - map->addend;
		if (run_reloc == 0x77777777)
			return 1;
		if (map->flags & PCREL)
			expected += run_addr + *run_o - offset;
		if (map->nameval->status == NOVAL) {
			map->nameval->val = expected;
			map->nameval->status = TEMP;
		} else if (map->nameval->val != expected) {
			if (rerun)
				return 1;
			printk("ksplice_h: pre-run reloc: Expected %s=%08x!\n",
			       map->nameval->name, expected);
			return 1;
		}
	}

	*pre_o += 4 - offset - 1;
	*run_o += 4 - offset - 1;
	return 0;
}

/* TODO: The recommended way to pad 64bit code is to use NOPs preceded by
   maximally four 0x66 prefixes.  */
int
match_nop(long addr, int *o, int *other_o)
{
	int i, j;
	for (i = NUM_NOPS - 1; i >= 0; i--) {
		for (j = 0; j < i + 1; j++) {
			if (!virtual_address_mapped(addr + *o + j))
				break;
			if (*(unsigned char *) (addr + *o + j) != nops[i][j])
				break;
		}
		if (j == i + 1) {
			*o += i;
			(*other_o)--;
			return 1;
		}

	}
	return 0;
}

void
brute_search_all_mods(struct ksplice_size *s)
{
	struct module *m;
	list_for_each_entry(m, &(THIS_MODULE->list), list) {
		if (!starts_with(m->name, ksplice_name)
		    && !ends_with(m->name, "_helper")) {
			if (brute_search(s, m->module_core, m->core_size) == 0)
				return;
			if (brute_search(s, m->module_init, m->init_size) == 0)
				return;
		}
	}
}
