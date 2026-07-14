#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>

#include "mite-opcodes.h"

typedef enum {
    FRAME_CALL,
    FRAME_LOOP,
    FRAME_DIP,
    FRAME_KEEP
} MiteFrameType;

typedef struct 
{
    uint16_t return_ip;
    uint16_t loop_ip;
    uint16_t saved_val;  
    uint8_t type;        
    bool wide;           
} MiteFrame;

typedef struct mite_vm
{
    uint8_t mem [(1<<16)-1];
    uint16_t ip;

    uint8_t stack [255];
    uint8_t top;

    uint8_t locals [255];
    uint8_t ltop;

    MiteFrame rstack [255];
    uint8_t rtop;

    void (*port_out) (uint8_t port, uint8_t arg);
    void (*port_in) (uint8_t port);
} MiteVM;

uint8_t
mite_local_pick (MiteVM *vm, uint8_t offset)
{
    return vm->locals[vm->ltop - 1 - offset];
}

void
mite_local_push (MiteVM *vm, uint8_t b)
{
    vm->locals[vm->ltop++] = b;
}

uint16_t
mite_pop1 (MiteVM *vm, bool wide, bool keep)
{
    if (wide) {
        uint16_t hi = vm->stack[vm->top - 1];
        uint16_t lo = vm->stack[vm->top - 2];
        if (!keep) vm->top -= 2;
        return lo | (hi << 8);
    } else {
        uint8_t b = vm->stack[vm->top - 1];
        if (!keep) vm->top -= 1;
        return b;
    }
}

void
mite_pop2 (MiteVM *vm, bool wide, bool keep, uint16_t *a, uint16_t *b)
{
    if (wide) {
        uint16_t a_hi = vm->stack[vm->top - 3];
        uint16_t a_lo = vm->stack[vm->top - 4];
        uint16_t b_hi = vm->stack[vm->top - 1];
        uint16_t b_lo = vm->stack[vm->top - 2];
        
        *a = a_lo | (a_hi << 8);
        *b = b_lo | (b_hi << 8);
        if (!keep) vm->top -= 4;
    } else {
        *a = vm->stack[vm->top - 2];
        *b = vm->stack[vm->top - 1];
        if (!keep) vm->top -= 2;
    }
}

void
mite_push1 (MiteVM *vm, bool wide, uint16_t val)
{
    if (wide) {
        vm->stack[vm->top++] = val & 0xff; // lo
        vm->stack[vm->top++] = val >> 8;   // hi
    } else {
        vm->stack[vm->top++] = (uint8_t)val;
    }
}

static void
mite_rpush (MiteVM *vm, uint16_t return_ip, uint16_t loop_ip, uint8_t type, uint16_t saved_val, bool wide)
{
    vm->rstack[vm->rtop++] = (MiteFrame) {
        return_ip,
        loop_ip,
        saved_val,
        type,
        wide
    };
}

MiteVM *
mite_init_fp (FILE *fp)
{
    MiteVM *vm = calloc (1, sizeof *vm);

    size_t start = ftell (fp);
    fseek (fp, 0, SEEK_END);
    size_t end = ftell (fp);
    fseek (fp, 0, SEEK_SET);
    size_t len = end - start;

    assert (fread (vm->mem, sizeof (uint8_t), len, fp) == len);

    vm->ip = 0;
    vm->top = 0;
    vm->ltop = 0;

    return vm;
}

void
mite_deinit (MiteVM *vm)
{
    free (vm);
}

void mite_run (MiteVM *vm);

void
port_out (uint8_t port, uint8_t arg)
{
    // printf ("OUT(%hhu): 0x%02X\n", port, arg);
    switch (port)
    {
    case 18: putc ((int)arg, stdout); fflush (stdout); break;
    }
}

int
main (int argc, char *argv[])
{
    if (argc <= 1)
    {
        fprintf (stderr, "USAGE: %s <PATH>\r\n", argv[0]);
        return 1;
    }
    
    const char *path = argv[1];

    FILE *fp = fopen (path, "rb");
    if (!fp)
    {
        perror ("fopen");
        return 2;
    }
    MiteVM *vm = mite_init_fp (fp);
    fclose (fp);

    vm->port_out = port_out;

    mite_run (vm);
    mite_deinit (vm);

    return 0;
}

static uint8_t
read_u8 (MiteVM *vm)
{
    assert (vm);
    return vm->mem[vm->ip++];
}

static uint16_t
read_u16 (MiteVM *vm)
{
    assert (vm);
    uint16_t lo = read_u8 (vm);
    uint16_t hi = read_u8 (vm);
    return lo | (hi << 8);
}

void
mite_run (MiteVM *vm)
{
    vm->ip = read_u16 (vm); // reset vector

    while (true)
    {
        uint8_t instruction = read_u8 (vm);
        uint8_t opcode = instruction & MITE_OPCODE_MASK;
        bool wide = instruction & MITE_FLAG_WIDE;
        bool keep = instruction & MITE_FLAG_KEEP;

        uint16_t a, b;

        switch (opcode)
        {
        case MITE_OP_HALT: return;
        
        case MITE_OP_RET:
        {
            if (vm->rtop == 0) return;
            MiteFrame *f = &vm->rstack[vm->rtop - 1];

            if (f->type == FRAME_LOOP) {
                uint8_t cond = (uint8_t)mite_pop1(vm, false, false); 
                if (cond) {
                    vm->ip = f->loop_ip;
                    break;
                }
            } 
            else if (f->type == FRAME_DIP || f->type == FRAME_KEEP)
                mite_push1(vm, f->wide, f->saved_val);

            vm->ip = f->return_ip;
            vm->rtop -= 1;
        }
        break;

        case MITE_OP_EVAL:
        {
            uint16_t target = mite_pop1(vm, true, keep);
            mite_rpush(vm, vm->ip, 0, FRAME_CALL, 0, false);
            vm->ip = target;
        }
        break;

        case MITE_OP_IFTE:
        {
            uint16_t else_br = mite_pop1(vm, true, false);
            uint16_t then_br = mite_pop1(vm, true, false);
            uint8_t cond = (uint8_t)mite_pop1(vm, false, keep);
            
            uint16_t target = cond ? then_br : else_br;
            mite_rpush(vm, vm->ip, 0, FRAME_CALL, 0, false);
            vm->ip = target;
        }
        break;

        case MITE_OP_LOOP:
        {
            uint16_t body = mite_pop1(vm, true, false);
            mite_rpush(vm, vm->ip, body, FRAME_LOOP, 0, false);
            vm->ip = body;
        }
        break;

        case MITE_OP_LIT:
        {
            uint16_t val = read_u8(vm);
            if (wide) val |= (read_u8(vm) << 8);
            mite_push1(vm, wide, val);
        }
        break;

        case MITE_OP_OUT: {
            uint8_t port = mite_pop1(vm, false, false); 
            uint8_t arg  = mite_pop1(vm, false, keep);  
            vm->port_out(port, arg);
            break;
        }

        case MITE_OP_LOAD:
        {
            uint16_t addr = mite_pop1(vm, true, keep);
            uint16_t val = vm->mem[addr];
            if (wide) val |= (vm->mem[addr + 1] << 8);
            mite_push1(vm, wide, val);
        }
        break;

        case MITE_OP_POKE: {
            uint16_t addr = mite_pop1(vm, true, false);
            uint8_t val = (uint8_t)mite_pop1(vm, false, false); // FIXME: wide?
            vm->mem[addr] = val;
        }
        break;

        case MITE_OP_DUP:
        {
            uint16_t val = mite_pop1(vm, wide, true);
            mite_push1(vm, wide, val);
        }
        break;

        case MITE_OP_EQ:
        {
            mite_pop2(vm, wide, keep, &a, &b);
            mite_push1(vm, false, a == b);
        }
        break;

        case MITE_OP_LT:
        {
            mite_pop2(vm, wide, keep, &a, &b);
            mite_push1(vm, false, a < b);
        }
        break;

        case MITE_OP_ADD:
        {
            mite_pop2(vm, wide, keep, &a, &b);
            mite_push1(vm, wide, a + b);
        }
        break;

        case MITE_OP_SUB:
        {
            mite_pop2(vm, wide, keep, &a, &b);
            mite_push1(vm, wide, a - b);
        }
        break;

        case MITE_OP_MOD:
        {
            mite_pop2(vm, wide, keep, &a, &b);
            mite_push1(vm, wide, a % b);
        }
        break;

        case MITE_OP_DIV:
        {
            mite_pop2(vm, wide, keep, &a, &b);
            mite_push1(vm, wide, a / b);
        }
        break;

        case MITE_OP_SWAP:
        {
            mite_pop2(vm, wide, keep, &a, &b);
            mite_push1(vm, wide, a);
            mite_push1(vm, wide, b);
        }
        break;

        case MITE_OP_OVER:
        {
            mite_pop2(vm, wide, true, &a, &b);
            mite_push1(vm, wide, b);
        }
        break;

        case MITE_OP_NOT:
            mite_push1(vm, wide, !mite_pop1(vm, wide, keep));
            break;

        case MITE_OP_NEG:
            mite_push1(vm, wide, ~mite_pop1(vm, wide, keep));
            break;

        case MITE_OP_DROP:
            mite_pop1(vm, wide, false);
            break;

        case MITE_OP_NIP:
            mite_pop2(vm, wide, false, &a, &b);
            mite_push1(vm, wide, b);
            break;

        case MITE_OP_TOL:
            if (wide) {
                uint16_t val = mite_pop1(vm, true, keep);
                mite_local_push(vm, val & 0xff);          // lo
                mite_local_push(vm, (val >> 8) & 0xff);   // hi
            } else {
                mite_local_push(vm, mite_pop1(vm, false, keep));
            }
            break;

        case MITE_OP_FROML:
        {
            uint8_t offset = read_u8(vm);
            if (wide) {
                uint8_t hi = mite_local_pick(vm, offset);
                uint8_t lo = mite_local_pick(vm, offset + 1);
                mite_push1(vm, true, (uint16_t)lo | ((uint16_t)hi << 8));
            } else {
                mite_push1(vm, false, mite_local_pick(vm, offset));
            }
        }
        break;
        
        case MITE_OP_DROPL:
            vm->ltop -= read_u8 (vm);
            break;
        
        case MITE_OP_DIP:
        {
            uint16_t target = mite_pop1(vm, true, false);
            uint16_t val    = mite_pop1(vm, wide, keep); // TODO: does keep make any sense here?
            mite_rpush(vm, vm->ip, 0, FRAME_DIP, val, wide);
            vm->ip = target;
        }
        break;

        case MITE_OP_KEEP:
        {
            uint16_t target = mite_pop1(vm, true, false);
            uint16_t val    = mite_pop1(vm, wide, keep); // TODO: does keep make any sense here?
            
            mite_push1 (vm, wide, val);

            mite_rpush(vm, vm->ip, 0, FRAME_KEEP, val, wide);
            vm->ip = target;
        }
        break;

        default:
            fprintf (stderr, "Opcode 0x%02X not implemented\n", opcode); 
            return;
        }
    }
}
