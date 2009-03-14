/*
 *  Copyright (C) 2004  The revenge Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 *
 * 11-9-2004 Initial work.
 *   Copyright (C) 2004 James Courtier-Dutton James@superbug.co.uk
 * 10-11-2007 Updates.
 *   Copyright (C) 2007 James Courtier-Dutton James@superbug.co.uk
 */

/* Intel ia32 instruction format: -
 Instruction-Prefixes (Up to four prefixes of 1-byte each. [optional] )
 Opcode (1-, 2-, or 3-byte opcode)
 ModR/M (1 byte [if required] )
 SIB (Scale-Index-Base:1 byte [if required] )
 Displacement (Address displacement of 1, 2, or 4 bytes or none)
 Immediate (Immediate data of 1, 2, or 4 bytes or none)

 Naming convention taked from Intel Instruction set manual,
 Appendix A. 25366713.pdf
*/

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <revenge/dis.h>
#include <revenge/bfl.h>
#include <revenge/exe.h>
#include <dis-asm.h>

#define EIP_START 0x40000000

instructions_t instructions;
uint8_t *inst;
size_t inst_size = 0;
struct rev_eng *handle;
struct disassemble_info disasm_info;
disassembler_ftype disassemble_fn;
char *dis_flags_table[] = { " ", "f" };
uint64_t inst_log = 1;	/* Pointer to the current free instruction log entry. */
char out_buf[1024];
int local_counter = 1;
void *self = NULL;

char *condition_table[] = {
	"Unknown_0",
	"OVERFLOW_1",
	"NOT_OVERFLOW_2",
	"BELOW_3",
	"NOT_BELOW_4",
	"EQUAL_5",
	"NOT_EQUAL_6",
	"ABOVE_7",
	"NOT_ABOVE_8",
	"SIGNED_9",
	"NO_SIGNED_10",
	"PARITY_11",
	"NOT_PARITY_12",
	"LESS_13",
	"GREATER_EQUAL_14",
	"LESS_EQUAL_15",
	"GREATER_16"
};

/* For the .data segment. I.e. Static data */
struct memory_s memory_data[1000];
/* For the .text segment. I.e. Instructions. */
struct memory_s memory_text[1000];
struct memory_s memory_reg[100];
struct memory_s memory_stack[100];

/* Used to store details of each instruction.
 * Linked by prev/next pointers
 * so that a single list can store all program flow.
 */
struct inst_log_entry_s inst_log_entry[100];

/* Used to keep record of where we have been before.
 * Used to identify program flow, branches, and joins.
 */
int memory_used[100];

struct entry_point_s {
	uint64_t eip_offset_value;
	uint64_t previous_instuction;
} ;

/* This is used to hold entry points into the code */
struct entry_point_s base_entry_point[100];
int base_entry_point_list_length;
/* This is used to hold return values from process block */
struct entry_point_s entry_point[2];
uint64_t entry_point_list_length;

	

int print_inst(struct instruction_s *instruction, int instruction_number)
{
	int ret = 1; /* Default to failed */
	printf("Instruction %d:%s%s",
		instruction_number,
		opcode_table[instruction->opcode],
		dis_flags_table[instruction->flags]);
	if (instruction->opcode != IF) {
		if (instruction->srcA.indirect) {
			printf(" %s[%s0x%"PRIx64"]%s,",
				indirect_table[instruction->srcA.indirect],
				store_table[instruction->srcA.store],
				instruction->srcA.index,
				size_table[instruction->srcA.size]);
		} else {
			printf(" %s0x%"PRIx64"%s,",
			store_table[instruction->srcA.store],
			instruction->srcA.index,
			size_table[instruction->srcA.size]);
		}
		if (instruction->dstA.indirect) {
			printf(" %s[%s0x%"PRIx64"]%s\n",
				indirect_table[instruction->dstA.indirect],
				store_table[instruction->dstA.store],
				instruction->dstA.index,
				size_table[instruction->dstA.size]);
		} else {
			printf(" %s0x%"PRIx64"%s\n",
			store_table[instruction->dstA.store],
			instruction->dstA.index,
			size_table[instruction->dstA.size]);
		}
		ret = 0;
		goto print_inst_exit;
	}
	if (instruction->opcode == IF) {
		printf(" cond=%"PRIu64"", instruction->srcA.index);
		printf(" JMP-REL=0x%"PRIx64"\n", instruction->dstA.index);
		ret = 0;
		goto print_inst_exit;
	}
print_inst_exit:
	return ret;
}

int print_instructions(void)
{
	int n;
	struct instruction_s *instruction;
	struct inst_log_entry_s *inst_log1;
	for (n = 1; n <= inst_log; n++) {
		inst_log1 =  &inst_log_entry[n];
		instruction =  &inst_log1->instruction;
		if (print_inst(instruction, n))
			return 1;
		printf("init:%"PRIx64", %"PRIx64" -> %"PRIx64"\n",
			inst_log1->value1.init_value,
			inst_log1->value2.init_value,
			inst_log1->value3.init_value);
		printf("offset:%"PRIx64", %"PRIx64" -> %"PRIx64"\n",
			inst_log1->value1.offset_value,
			inst_log1->value2.offset_value,
			inst_log1->value3.offset_value);
		printf("indirect init:%"PRIx64", %"PRIx64" -> %"PRIx64"\n",
			inst_log1->value1.indirect_init_value,
			inst_log1->value2.indirect_init_value,
			inst_log1->value3.indirect_init_value);
		printf("indirect offset:%"PRIx64", %"PRIx64" -> %"PRIx64"\n",
			inst_log1->value1.indirect_offset_value,
			inst_log1->value2.indirect_offset_value,
			inst_log1->value3.indirect_offset_value);
		printf("value_type:0x%x, 0x%x -> 0x%x\n",
			inst_log1->value1.value_type,
			inst_log1->value2.value_type,
			inst_log1->value3.value_type);
		printf("value_scope:0x%x, 0x%x -> 0x%x\n",
			inst_log1->value1.value_scope,
			inst_log1->value2.value_scope,
			inst_log1->value3.value_scope);
		printf("value_id:0x%"PRIx64", 0x%"PRIx64" -> 0x%"PRIx64"\n",
			inst_log1->value1.value_id,
			inst_log1->value2.value_id,
			inst_log1->value3.value_id);
		if (inst_log1->prev_size > 0) {
			int n;
			for (n = 0; n < inst_log1->prev_size; n++) {
				printf("inst_prev:%d:%d\n",
					n,
					inst_log1->prev[n]);
			}
		}
		if (inst_log1->next_size > 0) {
			int n;
			for (n = 0; n < inst_log1->next_size; n++) {
				printf("inst_next:%d:%d\n",
					n,
					inst_log1->next[n]);
			}
		}
	}
}

int get_value_from_index(operand_t *operand, uint64_t *index)
{
	if (operand->indirect) {
		printf(" %s%s[%s0x%"PRIx64"],",
			size_table[operand->size],
			indirect_table[operand->indirect],
			store_table[operand->store],
			operand->index);
	} else {
		printf(" %s%s0x%"PRIx64",",
		size_table[operand->size],
		store_table[operand->store],
		operand->index);
	}
	return 1;
}

int ram_init(void)
{

}

int reg_init(void)
{
	/* esp */
	memory_reg[0].start_address = REG_SP;
	/* 4 bytes */
	memory_reg[0].length = 4;
	/* 1 - Known */
	memory_reg[0].init_value_type = 1;
	/* Initial value when first accessed */
	memory_reg[0].init_value = 0x10000;
	/* No offset yet */
	memory_reg[0].offset_value = 0;
	/* 0 - unknown,
	 * 1 - unsigned,
	 * 2 - signed,
	 * 3 - pointer,
	 * 4 - Instruction,
	 * 5 - Instruction pointer(EIP),
	 * 6 - Stack pointer.
	 */
	memory_reg[0].value_type = 6;
	memory_reg[0].ref_memory = 0;
	memory_reg[0].ref_log = 0;
	/* value_scope: 0 - unknown, 1 - Param, 2 - Local, 3 - Global */
	memory_reg[0].value_scope = 0;
	/* Each time a new value is assigned, this value_id increases */
	memory_reg[0].value_id = 0;
	/* valid: 0 - Entry Not used yet, 1 - Entry Used */
	memory_reg[0].valid = 1;

	/* ebp */
	memory_reg[1].start_address = REG_BP;
	/* 4 bytes */
	memory_reg[1].length = 4;
	/* 1 - Known */
	memory_reg[1].init_value_type = 1;
	/* Initial value when first accessed */
	memory_reg[1].init_value = 0x20000;
	/* No offset yet */
	memory_reg[1].offset_value = 0;
	/* 0 - unknown,
	 * 1 - unsigned,
	 * 2 - signed,
	 * 3 - pointer,
	 * 4 - Instruction,
	 * 5 - Instruction pointer(EIP),
	 * 6 - Stack pointer.
	 */
	memory_reg[1].value_type = 6;
	memory_reg[1].ref_memory = 0;
	memory_reg[1].ref_log = 0;
	/* value_scope: 0 - unknown, 1 - Param, 2 - Local, 3 - Global */
	memory_reg[1].value_scope = 0;
	/* Each time a new value is assigned, this value_id increases */
	memory_reg[1].value_id = 0;
	/* valid: 0 - entry Not used yet, 1 - entry Used */
	memory_reg[1].valid = 1;

	/* eip */
	memory_reg[2].start_address = REG_IP;
	/* 4 bytes */
	memory_reg[2].length = 4;
	/* 1 - Known */
	memory_reg[2].init_value_type = 1;
	/* Initial value when first accessed */
	memory_reg[2].init_value = EIP_START;
	/* No offset yet */
	memory_reg[2].offset_value = 0;
	/* 0 - unknown,
	 * 1 - unsigned,
	 * 2 - signed,
	 * 3 - pointer,
	 * 4 - Instruction,
	 * 5 - Instruction pointer(EIP),
	 * 6 - Stack pointer.
	 */
	memory_reg[2].value_type = 5;
	memory_reg[2].ref_memory = 0;
	memory_reg[2].ref_log = 0;
	/* value_scope: 0 - unknown, 1 - Param, 2 - Local, 3 - Global */
	memory_reg[2].value_scope = 3;
	/* Each time a new value is assigned, this value_id increases */
	memory_reg[2].value_id = 0;
	/* valid: 0 - entry Not used yet, 1 - entry Used */
	memory_reg[2].valid = 1;
}

int stack_init(void)
{
	int n = 0;
	/* eip on the stack */
	memory_stack[n].start_address = 0x10000;
	/* 4 bytes */
	memory_stack[n].length = 4;
	/* 1 - Known */
	memory_stack[n].init_value_type = 1;
	/* Initial value when first accessed */
	memory_stack[n].init_value = 0x0;
	/* No offset yet */
	memory_stack[n].offset_value = 0;
	/* 0 - unknown,
	 * 1 - unsigned,
	 * 2 - signed,
	 * 3 - pointer,
	 * 4 - Instruction,
	 * 5 - Instruction pointer(EIP),
	 * 6 - Stack pointer.
	 */
	memory_stack[n].value_type = 5;
	memory_stack[n].ref_memory = 0;
	memory_stack[n].ref_log = 0;
	/* value_scope: 0 - unknown, 1 - Param, 2 - Local, 3 - Global */
	memory_stack[n].value_scope = 0;
	/* Each time a new value is assigned, this value_id increases */
	memory_stack[n].value_id = 0;
	/* valid: 0 - Not used yet, 1 - Used */
	memory_stack[n].valid = 1;
	n++;

#if 0
	/* Param1 */
	memory_stack[n].start_address = 0x10004;
	/* 4 bytes */
	memory_stack[n].length = 4;
	/* 1 - Known */
	memory_stack[n].init_value_type = 1;
	/* Initial value when first accessed */
	memory_stack[n].init_value = 0x321;
	/* No offset yet */
	memory_stack[n].offset_value = 0;
	/* 0 - unknown,
	 * 1 - unsigned,
	 * 2 - signed,
	 * 3 - pointer,
	 * 4 - Instruction,
	 * 5 - Instruction pointer(EIP),
	 * 6 - Stack pointer.
	 */
	memory_stack[n].value_type = 2;
	memory_stack[n].ref_memory = 0;
	memory_stack[n].ref_log = 0;
	/* value_scope: 0 - unknown, 1 - Param, 2 - Local, 3 - Global */
	memory_stack[n].value_scope = 0;
	/* Each time a new value is assigned, this value_id increases */
	memory_stack[n].value_id = 0;
	/* valid: 0 - Not used yet, 1 - Used */
	memory_stack[n].valid = 1;
	n++;
#endif
}

int process_block( uint64_t inst_log_prev, uint64_t *list_length, struct entry_point_s *entry) {
	uint64_t offset = 0;
	int result;
	int n = 0;
	int err;
	struct inst_log_entry_s *inst_exe_prev;
	struct inst_log_entry_s *inst_exe;
	struct instruction_s *instruction;
	int instruction_offset = 0;
	int octets = 0;
	printf("process_block entry\n");
	printf("inst_log=%"PRId64"\n", inst_log);
	printf("dis:Data at %p, size=%"PRId32"\n", inst, inst_size);
	for (offset = 0; ;
			/* Update EIP */
			offset = memory_reg[2].offset_value
			) {
	//for (offset = 0; offset < inst_size;
			//offset += instructions.bytes_used) {
		offset = memory_reg[2].offset_value;
		instructions.instruction_number = 0;
		instructions.bytes_used = 0;
		printf("eip=0x%"PRIx64", offset=0x%"PRIx64"\n",
			memory_reg[2].offset_value, offset);
		result = disassemble(&instructions, &inst[offset]);
		printf("bytes used = %d\n", instructions.bytes_used);
		/* Memory not used yet */
		if (0 == memory_used[offset]) {
			printf("Memory not used yet\n");
			for (n = 0; n < instructions.bytes_used; n++) {
				memory_used[offset + n] = -n;
				printf(" 0x%02x", inst[offset + n]);
			}
			printf("\n");
			memory_used[offset] = inst_log;
		} else {
			int inst_this = memory_used[offset];
			/* If value == maxint, then it is the destination of a jump */
			/* But I need to separate the instruction flows */
			/* A jump/branch inst should create a new instruction tree */
			printf("Memory already used\n");
			inst_exe_prev = &inst_log_entry[inst_log_prev];
			inst_exe = &inst_log_entry[inst_this];
			printf("inst_exe_prev=%p, inst_exe=%p\n",
				inst_exe_prev, inst_exe);
			inst_exe->prev_size++;
			if (inst_exe->prev_size == 1) {
				inst_exe->prev = malloc(sizeof(inst_exe->prev));
			} else {
				inst_exe->prev = realloc(inst_exe->prev, sizeof(inst_exe->prev) * inst_exe->prev_size);
			}
			inst_exe->prev[inst_exe->prev_size - 1] = inst_log_prev;
			inst_exe_prev->next_size++;
			if (inst_exe_prev->next_size == 1) {
				inst_exe_prev->next = malloc(sizeof(inst_exe_prev->next));
				inst_exe_prev->next[inst_exe_prev->next_size - 1] = inst_this;
			} else {
				inst_exe_prev->next = realloc(inst_exe_prev->next, sizeof(inst_exe_prev->next) * inst_exe_prev->next_size);
				inst_exe_prev->next[inst_exe_prev->next_size - 1] = inst_this;
			}
			break;
		}	
		//printf("disassemble_fn\n");
		//disassemble_fn = disassembler (handle->bfd);
		//printf("disassemble_fn done\n");
		printf("disassemble: ");
		octets = (*disassemble_fn) (offset, &disasm_info);
		printf("  octets=%d\n", octets);
		if (instructions.bytes_used != octets) {
			printf("Unhandled instruction. Length mismatch. Exiting\n");
			return 1;
		}
		/* Update EIP */
		memory_reg[2].offset_value += octets;

		printf("Number of RTL instructions=%d\n",
			instructions.instruction_number);
		if (result == 0) {
			printf("Unhandled instruction. Exiting\n");
			return 1;
		}
		if (instructions.instruction_number == 0) {
			printf("NOP instruction. Get next inst\n");
			continue;
		}
		for (n = 0; n < instructions.instruction_number; n++) {
			instruction = &instructions.instruction[n];
			printf( "Printing inst1111:%d, %d, %"PRId64"\n",instruction_offset, n, inst_log);
			err = print_inst(instruction, instruction_offset + n + 1);
			if (err) {
				printf("print_inst failed\n");
				return err;
			}
			inst_exe_prev = &inst_log_entry[inst_log_prev];
			inst_exe = &inst_log_entry[inst_log];
			memcpy(&(inst_exe->instruction), instruction, sizeof(struct instruction_s));
			err = execute_instruction(self, inst_exe);
			if (err) {
				printf("execute_intruction failed err=%d\n", err);
				return err;
			}
			inst_exe->prev_size++;
			if (inst_exe->prev_size == 1) {
				inst_exe->prev = malloc(sizeof(inst_exe->prev));
			} else {
				inst_exe->prev = realloc(inst_exe->prev, sizeof(inst_exe->prev) * inst_exe->prev_size);
			}
			inst_exe->prev[inst_exe->prev_size - 1] = inst_log_prev;
			inst_exe_prev->next_size++;
			if (inst_exe_prev->next_size == 1) {
				inst_exe_prev->next = malloc(sizeof(inst_exe_prev->next));
				inst_exe_prev->next[inst_exe_prev->next_size - 1] = inst_log;
			} else {
				inst_exe_prev->next = realloc(inst_exe_prev->next, sizeof(inst_exe_prev->next) * inst_exe_prev->next_size);
				inst_exe_prev->next[inst_exe_prev->next_size - 1] = inst_log;
			}
			inst_exe_prev->next[inst_exe_prev->next_size - 1] = inst_log;

			inst_log_prev = inst_log;
			inst_log++;
			if (0 == memory_reg[2].offset_value) {
				printf("Function exited\n");
				break;
			}
		}
		instruction_offset += instructions.instruction_number;
		if (0 == memory_reg[2].offset_value) {
			printf("Breaking\n");
			break;
		}
		if (IF == instruction->opcode) {
			printf("Breaking at IF\n");
			printf("IF: this EIP = 0x%"PRIx64"\n",
				memory_reg[2].offset_value);
			printf("IF: jump dst abs EIP = 0x%"PRIx64"\n",
				inst_exe->value3.offset_value);
			printf("IF: inst_log = %"PRId64"\n",
				inst_log);
			*list_length = 2;
			entry[0].eip_offset_value = memory_reg[2].offset_value;
			entry[0].previous_instuction = inst_log - 1;
			entry[1].eip_offset_value = inst_exe->value3.offset_value;
			entry[1].previous_instuction = inst_log - 1;
			break;
		}
	}
	return 0;
}

int output_variable(int store, uint64_t index, uint64_t value_scope, uint64_t value_id, uint64_t indirect_offset_value, char *out_buf, int *write_offset) {
	int tmp;
	switch (store) {
	case STORE_IMMED:
		printf("%"PRIx64";\n", index);
		tmp = snprintf(out_buf + *write_offset, 1024 - *write_offset, "0x%"PRIx64" ",
			index);
		*write_offset += tmp;
		break;
	case STORE_REG:
		if ((value_scope) == 2) {
			printf("local%04"PRIu64";\n", (value_id));
			tmp = snprintf(out_buf + *write_offset, 1024 - *write_offset, "local%04"PRIu64" ",
				value_id);
			*write_offset += tmp;
		} else {
			printf("param%04"PRIu64";\n", (indirect_offset_value));
			tmp = snprintf(out_buf + *write_offset, 1024 - *write_offset, "param%04"PRIu64" ",
				indirect_offset_value);
			*write_offset += tmp;
			printf("write_offset=%d\n", *write_offset);
		}
		break;
	case STORE_MEM:
	case STORE_STACK:
	default:
		printf("Unhandled store1\n");
		break;
	}
}


int if_expression( int condition, struct inst_log_entry_s *inst_log1_flagged, char *out_buf, int *write_offset)
{
	int opcode = inst_log1_flagged->instruction.opcode;
	int err = 0;
	int tmp;
	int store;
	uint64_t index;
	uint64_t value_scope;
	uint64_t value_id;
	uint64_t indirect_offset_value;

	switch (opcode) {
	case CMP:
		switch (condition) {
		case LESS_EQUAL:
			tmp = snprintf(out_buf + *write_offset, 999 - *write_offset, "( ");
			*write_offset += tmp;
			store = inst_log1_flagged->instruction.dstA.store;
			index = inst_log1_flagged->instruction.dstA.index;
			value_scope = inst_log1_flagged->value2.value_scope;
			value_id = inst_log1_flagged->value2.value_id;
			indirect_offset_value = inst_log1_flagged->value2.indirect_offset_value;
			tmp = output_variable(store, index, value_scope, value_id, indirect_offset_value, out_buf, write_offset);
			tmp = snprintf(out_buf + *write_offset, 999 - *write_offset, "<= "
				);
			*write_offset += tmp;
			store = inst_log1_flagged->instruction.srcA.store;
			index = inst_log1_flagged->instruction.srcA.index;
			value_scope = inst_log1_flagged->value1.value_scope;
			value_id = inst_log1_flagged->value1.value_id;
			indirect_offset_value = inst_log1_flagged->value1.indirect_offset_value;
			tmp = output_variable(store, index, value_scope, value_id, indirect_offset_value, out_buf, write_offset);
			tmp = snprintf(out_buf + *write_offset, 999 - *write_offset, ") ");
			*write_offset += tmp;
			break;
		default:
			err = 1;
			break;
		break;
		}
	default:
		err = 1;
		break;
	}
	return err;
}

int main(int argc, char *argv[])
{
	int n = 0;
	uint64_t offset = 0;
	int instruction_offset = 0;
	int octets = 0;
	int result;
	char *filename;
	int fd;
	int tmp;
	int err;
	const char *file = "test.obj";
	size_t inst_size = 0;
	int l;
	struct instruction_s *instruction;
	struct instruction_s *instruction_prev;
	struct inst_log_entry_s *inst_log1;
	struct inst_log_entry_s *inst_log1_prev;
	struct inst_log_entry_s *inst_exe;
	struct inst_log_entry_s *inst_exe_prev;
	struct memory_s *value;
	uint64_t inst_log_prev = 0;
	char *expression;
	ram_init();
	reg_init();
	stack_init();

	expression = malloc(1000); /* Buffer for if expressions */

	handle = bf_test_open_file(file);
	if (!handle) {
		printf("Failed to find or recognise file\n");
		return 1;
	}

	printf("symtab_canon2 = %ld\n", handle->symtab_sz);
	for (l = 0; l < handle->symtab_sz; l++) {
		printf("%d\n", l);
		printf("type:0x%02x\n", handle->symtab[l]->flags);
		printf("name:%s\n", handle->symtab[l]->name);
		printf("value=0x%02"PRIx64"\n", handle->symtab[l]->value);
	}
	printf("Setup ok\n");
	inst_size = bf_get_code_size(handle);
	inst = malloc(inst_size);
	bf_copy_code_section(handle, inst, inst_size);
	printf("dis:Data at %p, size=%"PRId32"\n", inst, inst_size);
	for (n = 0; n < inst_size; n++) {
		printf(" 0x%02x", inst[n]);
	}
	printf("\n");

	printf("handle=%p\n", handle);
	init_disassemble_info(&disasm_info, stdout, (fprintf_ftype) fprintf);
	disasm_info.flavour = bfd_get_flavour(handle->bfd);
	disasm_info.arch = bfd_get_arch(handle->bfd);
	disasm_info.mach = bfd_get_mach(handle->bfd);
	disasm_info.disassembler_options = NULL;
	disasm_info.octets_per_byte = bfd_octets_per_byte(handle->bfd);
	disasm_info.skip_zeroes = 8;
	disasm_info.skip_zeroes_at_end = 3;
	disasm_info.disassembler_needs_relocs = 0;
	disasm_info.buffer_length = inst_size;
	disasm_info.buffer = inst;

	printf("disassemble_fn\n");
	disassemble_fn = disassembler(handle->bfd);
	printf("disassemble_fn done %p, %p\n", disassemble_fn, print_insn_i386);
	instructions.bytes_used = 0;
	inst_exe = &inst_log_entry[0];
	/* EIP is a parameter for process_block */
	/* Update EIP */
	memory_reg[2].offset_value = 0;
	inst_log_prev = 0;
	err = process_block( inst_log_prev, &entry_point_list_length, entry_point);
	if (err) {
		printf("process_block failed\n");
		return err;
	}
	if (entry_point_list_length > 0) {
		for (n = 0; n < entry_point_list_length; n++ ) {
			printf("eip = 0x%"PRIx64", prev_inst = 0x%"PRIx64"\n",
				entry_point[n].eip_offset_value,
				entry_point[n].previous_instuction);
		}
	}
	//inst_log--;
	printf("Instructions=%"PRId64", entry_point_list_length=%"PRId64"\n",
		inst_log,
		entry_point_list_length);
	for (n = 0; n < entry_point_list_length; n++ ) {
		/* EIP is a parameter for process_block */
		/* Update EIP */
		memory_reg[2].offset_value = entry_point[n].eip_offset_value;
		inst_log_prev = entry_point[n].previous_instuction;
		err = process_block( inst_log_prev, &entry_point_list_length, entry_point);
		if (err) {
			printf("process_block failed\n");
			return err;
		}
	}
	/* Correct inst_log to identify how many instructions there have been */
	inst_log--;
	print_instructions();
	if (entry_point_list_length > 0) {
		for (n = 0; n < entry_point_list_length; n++ ) {
			printf("eip = 0x%"PRIx64", prev_inst = 0x%"PRIx64"\n",
				entry_point[n].eip_offset_value,
				entry_point[n].previous_instuction);
		}
	}
	print_instructions();
	filename = "test.c";
	fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		printf("Failed to open file %s, error=%d\n", filename, fd);
		return 1;
	}
	printf("fd=%d\n", fd);
	printf("writing out to file\n");
	/* Output function name */
	for (l = 0; l < handle->symtab_sz; l++) {
		if (handle->symtab[l]->flags == 0x12) {
			printf("int %s()\n{\n", handle->symtab[l]->name);
			tmp = snprintf(out_buf, 1024, "int %s()\n{\n", handle->symtab[l]->name);
			write(fd, out_buf, tmp);
		}
		//printf("type:0x%02x\n", handle->symtab[l]->flags);
		//printf("name:%s\n", handle->symtab[l]->name);
		//printf("value=0x%02x\n", handle->symtab[l]->value);
	}
	for (n = 1; n <= inst_log; n++) {
		int write_offset = 0;
		inst_log1 =  &inst_log_entry[n];
		inst_log1_prev =  &inst_log_entry[inst_log1->prev[0]];
		instruction =  &inst_log1->instruction;
		instruction_prev =  &inst_log1_prev->instruction;
		if ((inst_log1->prev_size) > 1) {
					printf("label%04"PRIx32":\n", n);
					tmp = snprintf(out_buf + write_offset, 1024 - write_offset,
						"label%04"PRIx32":\n", n);
					write_offset += tmp;
		}
		/* Test to see if we have an instruction to output */
		printf("Inst %d: %d: value_scope = %d, %d, %d\n", n,
			instruction->opcode,
			inst_log1->value1.value_scope,
			inst_log1->value2.value_scope,
			inst_log1->value3.value_scope);
		if ((0 == inst_log1->value3.value_type) ||
			(1 == inst_log1->value3.value_type) ||
			(2 == inst_log1->value3.value_type) ||
			(3 == inst_log1->value3.value_type) ||
			(5 == inst_log1->value3.value_type)) {
			switch (instruction->opcode) {
			case MOV:
				if (inst_log1->value1.value_type == 6)
					break;
				/* FIXME: This might need adding back in when
				 *        complex EIP jumps happen
				 */
				if (inst_log1->value1.value_type == 5)
					break;
				if (print_inst(instruction, n))
					return 1;
				printf("\t");
				tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "\t");
				write_offset += tmp;
				if ((inst_log1->value3.value_scope) == 2) {
					printf("local%04"PRIu64" = ", (inst_log1->value3.value_id));
					tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "local%04"PRIu64" = ",
						inst_log1->value3.value_id);
					write_offset += tmp;
				} else {
					printf("param%04"PRIu64" = ", (inst_log1->value3.indirect_offset_value));
					tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "param%04"PRIu64" = ",
						inst_log1->value3.indirect_offset_value);
					write_offset += tmp;
				}
				printf("\nstore=%d\n", instruction->srcA.store);
				switch (instruction->srcA.store) {
				case STORE_IMMED:
					printf("%"PRIx64";\n", instruction->srcA.index);
					tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "0x%"PRIx64";\n",
						instruction->srcA.index);
					write_offset += tmp;
					break;
				case STORE_REG:
					if ((inst_log1->value1.value_scope) == 2) {
						printf("local%04"PRIu64";\n", (inst_log1->value1.value_id));
						tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "local%04"PRIu64";\n",
							inst_log1->value1.value_id);
						write_offset += tmp;
					} else {
						printf("param%04"PRIu64";\n", (inst_log1->value1.indirect_offset_value));
						tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "param%04"PRIu64";\n",
							inst_log1->value1.indirect_offset_value);
						write_offset += tmp;
						printf("write_offset=%d\n", write_offset);
					}
					break;
				case STORE_MEM:
				case STORE_STACK:
				default:
					printf("Unhandled store1\n");
					break;
				}
				break;
			case ADD:
				if (print_inst(instruction, n))
					return 1;
				printf("\t");
				tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "\t");
				write_offset += tmp;
				if ((inst_log1->value3.value_scope) == 2) {
					printf("local%04"PRIu64" += ", (inst_log1->value3.value_id));
					tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "local%04"PRIu64" += ",
						inst_log1->value3.value_id);
					write_offset += tmp;
				} else {
					printf("param%04"PRIu64" += ", (inst_log1->value3.indirect_offset_value));
					tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "param%04"PRIu64" += ",
						inst_log1->value3.indirect_offset_value);
					write_offset += tmp;
				}
				printf("\nstore=%d\n", instruction->srcA.store);
				switch (instruction->srcA.store) {
				case STORE_IMMED:
					printf("%"PRIx64";\n", instruction->srcA.index);
					tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "0x%"PRIx64";\n",
						instruction->srcA.index);
					write_offset += tmp;
					break;
				case STORE_REG:
					if ((inst_log1->value1.value_scope) == 2) {
						printf("local%04"PRIu64";\n", (inst_log1->value1.value_id));
						tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "local%04"PRIu64";\n",
							inst_log1->value1.value_id);
						write_offset += tmp;
					} else {
						printf("param%04"PRIu64";\n", (inst_log1->value1.indirect_offset_value));
						tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "param%04"PRIu64";\n",
							inst_log1->value1.indirect_offset_value);
						write_offset += tmp;
						printf("write_offset=%d\n", write_offset);
					}
					break;
				case STORE_MEM:
				case STORE_STACK:
				default:
					printf("Unhandled store1\n");
					break;
				}
				break;
			case SUB:
				if (print_inst(instruction, n))
					return 1;
				printf("\t");
				tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "\t");
				write_offset += tmp;
				if ((inst_log1->value3.value_scope) == 2) {
					printf("local%04"PRIu64" -= ", (inst_log1->value3.value_id));
					tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "local%04"PRIu64" -= ",
						inst_log1->value3.value_id);
					write_offset += tmp;
				} else {
					printf("param%04"PRIu64" -= ", (inst_log1->value3.indirect_offset_value));
					tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "param%04"PRIu64" -= ",
						inst_log1->value3.indirect_offset_value);
					write_offset += tmp;
				}
				printf("\nstore=%d\n", instruction->srcA.store);
				switch (instruction->srcA.store) {
				case STORE_IMMED:
					printf("%"PRIx64";\n", instruction->srcA.index);
					tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "0x%"PRIx64";\n",
						instruction->srcA.index);
					write_offset += tmp;
					break;
				case STORE_REG:
					if ((inst_log1->value1.value_scope) == 2) {
						printf("local%04"PRIu64";\n", (inst_log1->value1.value_id));
						tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "local%04"PRIu64";\n",
							inst_log1->value1.value_id);
						write_offset += tmp;
					} else {
						printf("param%04"PRIu64";\n", (inst_log1->value1.indirect_offset_value));
						tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "param%04"PRIu64";\n",
							inst_log1->value1.indirect_offset_value);
						write_offset += tmp;
						printf("write_offset=%d\n", write_offset);
					}
					break;
				case STORE_MEM:
				case STORE_STACK:
				default:
					printf("Unhandled store1\n");
					break;
				}
				break;
			case JMP:
				printf("JMP reached XXXX\n");
				if (print_inst(instruction, n))
					return 1;
				tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "\t");
				write_offset += tmp;
				printf("goto label%04"PRIx32";\n}\n",
					inst_log1->next[0]);
				tmp = snprintf(out_buf + write_offset, 1024 - write_offset,
					"goto label%04"PRIx32";\n}\n",
					inst_log1->next[0]);
				write_offset += tmp;
				break;
			case CMP:
				/* Don't do anything for this instruction. */
				/* only does anything if combined with a branch instruction */
				if (print_inst(instruction, n))
					return 1;
				tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "\t");
				write_offset += tmp;
				tmp = snprintf(out_buf + write_offset, 1024 - write_offset,
					"/* cmp; */\n");
				write_offset += tmp;
				printf("/* cmp; */\n");
				break;

			case IF:
				/* FIXME: Never gets here, why? */
				/* Don't do anything for this instruction. */
				/* only does anything if combined with a branch instruction */
				if (print_inst(instruction, n))
					return 1;
				printf("\t");
				tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "\t");
				write_offset += tmp;
				printf("if ");
				tmp = snprintf(out_buf + write_offset, 1024 - write_offset,
					"if ");
				write_offset += tmp;
				err = if_expression( instruction->srcA.index, inst_log1_prev, out_buf, &write_offset);
				printf("\t prev=%d, ",inst_log1->prev[0]);
				printf("\t prev inst=%d, ",instruction_prev->opcode);
				printf("\t %s", condition_table[instruction->srcA.index]);
				printf("\t %s", expression);
				printf("\t LHS=%d, ",inst_log1->prev[0]);
				tmp = snprintf(out_buf + write_offset, 1024 - write_offset,
					"%s", expression);
				write_offset += tmp;
				printf("goto label%04"PRIx32";\n", inst_log1->next[1]);
				tmp = snprintf(out_buf + write_offset, 1024 - write_offset,
					"goto label%04"PRIx32";\n", inst_log1->next[1]);
				write_offset += tmp;
				break;

			default:
				printf("Unhandled output instruction1\n");
				if (print_inst(instruction, n))
					return 1;
				return 1;
				break;
			}
		}
		if (write_offset) {
			out_buf[write_offset] = 0;
			printf("cmd:%d:%s\n", write_offset, out_buf);
			write(fd, out_buf, write_offset);
		}
		write_offset = 0;
		printf("GOT HERE1. value_type=%d\n",
			inst_log1->value3.value_type);
		printf("GOT HERE2. dstA.indirect=%d\n",
			instruction->dstA.indirect);
		printf("GOT HERE3. dstA.store=%x cmp %x\n",
			instruction->dstA.store, STORE_REG);
		printf("GOT HERE4. dstA.index=%"PRIx64" cmp %x\n",
			instruction->dstA.index, REG_IP);
		if ((5 == inst_log1->value3.value_type) &&
			(!instruction->dstA.indirect) &&
			(instruction->dstA.store == STORE_REG) &&
			(instruction->dstA.index ==  REG_IP)) {
			/* FIXME: select correct return variable */
			/* Search for EAX */
			value = search_store(memory_reg,
					4, 4);
			printf("\treturn local%04"PRId64";\n}\n", value->value_id);
			tmp = snprintf(out_buf + write_offset, 1024 - write_offset, "\treturn local%04"PRId64";\n}\n", value->value_id);
			write_offset += tmp;
			write(fd, out_buf, write_offset);
			write_offset = 0;
		}
		//tmp = snprintf(out_buf, 1024, "%d\n", l);
		//write(fd, out_buf, tmp);
	}
	close(fd);
	bf_test_close_file(handle);
	for (n = 0; n < inst_size; n++) {
		printf("0x%04x: %d\n", n, memory_used[n]);
	}
	printf("\n");
	return 0;
}

