#include "opcode.h"

// 2. Create functions or macros to set/get the fields.
uint32_t set_opcode(uint32_t opcode, uint32_t value, uint32_t mask, uint8_t shift) {
    // Clear the bits for the field, then set them with the new value
    opcode &= ~(mask << shift); // Clear the bits for the field
    return opcode | ((value & mask) << shift);
}

uint32_t get_opcode(uint32_t opcode, uint32_t mask, uint8_t shift) {
    return (opcode >> shift) & mask;
}

uint32_t getRemainingValue(uint32_t data) {
    // Apply the MASK_26BITS (0x03FFFFFF) to clear the opcode bits
    // and keep only the value bits.
    return data & MASK_26BITS;
}