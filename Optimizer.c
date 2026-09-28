#include "Optimizer.h"
#include "opcode.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void freeASTNode(ASTNode* node);

// Helper to check if a node is a constant number
bool isNumberConst(ASTNode* node) {
    if (!node || node->type != AST_CONST) return false;
    TypeIDEnum t = ((ASTConst*)node)->value.type.baseType;
    return (t == T_INT || t == T_FLOAT);
}

// Forward declaration of your factory if not in header
// extern ASTNode* createASTConst(CompilerDef* compiler, Value value, Loc loc);
static ASTNode* fold_node(CompilerDef* compiler, ASTNode* node) {
    if (!node) return NULL;

    switch (node->type) {
    case AST_BLOCK: {
        ASTBlock* block = (ASTBlock*)node;
        // Recursively fold all statements in the block
        for (int i32 = 0; i32 < array_size(&block->list); ++i32) {
            ASTNode** stmtPtr = (ASTNode**)array_get(&block->list, i32);
            *stmtPtr = fold_node(compiler, *stmtPtr);
        }
        return node;
    }

    case AST_RETURN: {
        ASTReturn* ret = (ASTReturn*)node;
        if (ret->retval) {
            ret->retval = fold_node(compiler, ret->retval);
        }
        return node;
    }

    case AST_FUNC_CALL: {
        ASTFuncCall* call = (ASTFuncCall*)node;
        for (int i32 = 0; i32 < array_size(&call->arguments); ++i32) {
            ASTNode** arg = (ASTNode**)array_get(&call->arguments, i32);
            *arg = fold_node(compiler, *arg);
        }
        return node;
    }

    case AST_IF: {
        ASTIfStmt* ifStmt = (ASTIfStmt*)node;
        ifStmt->condition = fold_node(compiler, ifStmt->condition);
        if (isNumberConst(ifStmt->condition)) {            
            ASTConst* condConst = (ASTConst*)ifStmt->condition;
            bool condVal = false;
            if (condConst->value.type.baseType == T_INT)
                condVal = condConst->value.v.i32 != 0;
            else if (condConst->value.type.baseType == T_FLOAT)
                condVal = condConst->value.v.f32 != 0.0f;
            
            if (condVal != 0) {  // True: Replace with thenBlock
                ASTNode* then = ifStmt->thenBlock;
                // AST nodes are arena-owned and remain registered in
                // CompilerDef::ASTs. Do not free or destroy their child
                // arrays here; bulk cleanup owns them.
                return fold_node(compiler, then);
            }
            else {  // False: Replace with elseBlock or empty
                ASTNode* els = ifStmt->elseBlock;
                return els ? fold_node(compiler, els) : NULL;
            }
        }
        // Else recurse normally
        ifStmt->thenBlock = fold_node(compiler, ifStmt->thenBlock);
        if (ifStmt->elseBlock) ifStmt->elseBlock = fold_node(compiler, ifStmt->elseBlock);
        return node;
    }

    case AST_UNARY_OP: {
        ASTUnaryOp* un = (ASTUnaryOp*)node;
        // 1. Fold Child
        un->operand = fold_node(compiler, un->operand);

        // 2. Attempt Folding
        if (un->operand && un->operand->type == AST_CONST) {
            ASTConst* c = (ASTConst*)un->operand;
            Value newVal = c->value;
            bool folded = false;

            if (un->op == TK_MINUS) {
                if (c->value.type.baseType == T_INT) 
                { 
					newVal.type = c->value.type;
                    newVal.v.i32 = -c->value.v.i32; 
                    folded = true; 
                }
                else if (c->value.type.baseType == T_FLOAT) { 
                    newVal.type = c->value.type;
                    newVal.v.f32 = -c->value.v.f32; 
                    folded = true; 
                }
            }
            else if (un->op == TK_NOT && c->value.type.baseType == T_BOOL) {
                newVal.v.i32 = !c->value.v.i32; // Logical NOT
                newVal.type = c->value.type;
                folded = true;
            }

            if (folded) {
                // Create NEW node, registered in compiler->ASTs
                // createASTConst is variadic and expects the raw value (int/float/char*/etc.),
                // not a Value struct. Pass the correct member based on the type.
                if (newVal.type.baseType == T_INT || newVal.type.baseType == T_BOOL)
                    return createASTConst(compiler, node->loc, newVal.type.baseType, newVal.v.i32);
                else if (newVal.type.baseType == T_FLOAT)
                    return createASTConst(compiler, node->loc, newVal.type.baseType, newVal.v.f32);
                else if (newVal.type.baseType == T_STRING)
                    return createASTConst(compiler, node->loc, newVal.type.baseType, (char*)newVal.v.p);
                else
                    return createASTConst(compiler, node->loc, newVal.type.baseType, newVal.v.i32);
                // Old 'node' is abandoned (not freed), cleaned up by compiler later
            }
        }
        return node;
    }

    case AST_BINARY_OP: {
        ASTBinaryOp* bin = (ASTBinaryOp*)node;

        // 1. Fold Children First (Bottom-Up)
        bin->left = fold_node(compiler, bin->left);
        bin->right = fold_node(compiler, bin->right);

        // 2. Check for Constants
        if (bin->left && bin->right) {
            if (bin->left && bin->right && bin->left->type == AST_CONST && bin->right->type == AST_CONST) {
                ASTConst* l = (ASTConst*)bin->left;
                ASTConst* r = (ASTConst*)bin->right;

                if (l->value.type.baseType != r->value.type.baseType) return node;

                Value res = l->value;
                bool folded = false;

                if (l->value.type.baseType == T_INT) {
                    switch (bin->op) {
                    case TK_PLUS:  res.v.i32 = l->value.v.i32 + r->value.v.i32; folded = true; break;
                    case TK_MINUS: res.v.i32 = l->value.v.i32 - r->value.v.i32; folded = true; break;
                    case TK_MULT:  res.v.i32 = l->value.v.i32 * r->value.v.i32; folded = true; break;
                    case TK_DIV:   
                        if (r->value.v.i32 != 0) { res.v.i32 = l->value.v.i32 / r->value.v.i32; folded = true; } 
                        res.type = l->value.type;
                        break;
                    default: break;
                    }
                }
                else if (l->value.type.baseType == T_FLOAT) {
                    switch (bin->op) {
                    case TK_PLUS:  res.v.f32 = l->value.v.f32 + r->value.v.f32; folded = true; res.type = l->value.type; break;
                    case TK_MINUS: res.v.f32 = l->value.v.f32 - r->value.v.f32; folded = true; res.type = l->value.type; break;
                    case TK_MULT:  res.v.f32 = l->value.v.f32 * r->value.v.f32; folded = true; res.type = l->value.type; break;
                    case TK_DIV:   
                        if (r->value.v.f32 != 0.0) { res.v.f32 = l->value.v.f32 / r->value.v.f32; folded = true; } 
                        res.type = l->value.type;
                               break;
                    default: break;
                    }
                }

                if (folded) {
                    // Return new Constant. The old BinaryOp node is abandoned (garbage collected later).
                    if (res.type.baseType == T_INT)
                        return createASTConst(compiler, node->loc, res.type.baseType, res.v.i32);
                    else if (res.type.baseType == T_FLOAT)
                    {
                        return createASTConst(compiler, node->loc, res.type.baseType, res.v.f32);
                    }
                }
            }

            // 3. Algebraic Identities (x + 0, x * 1)
            // Note: We cannot simply return 'bin->left' because 'bin' owns the structure.
            // But since we are allowed to abandon nodes, we CAN just return the child!

            if (bin->right->type == AST_CONST) {
                ASTConst* r = (ASTConst*)bin->right;
                if (r->value.type.baseType == T_INT) {
                    // x + 0 = x
                    if (bin->op == TK_PLUS && r->value.v.i32 == 0) return bin->left;
                    // x * 1 = x
                    if (bin->op == TK_MULT && r->value.v.i32 == 1) return bin->left;
                    // x * 0 = 0
                    if (bin->op == TK_MULT && r->value.v.i32 == 0) {
                        return createASTConst(compiler, node->loc, T_INT, r->value.v.i32);
                    }
                }
                else if (r->value.type.baseType == T_FLOAT) {
                    // x + 0.0 = x
                    if (bin->op == TK_PLUS && r->value.v.f32 == 0.0) return bin->left;
                    // x * 1.0 = x
                    if (bin->op == TK_MULT && r->value.v.f32 == 1.0) return bin->left;
                }
            }
        }
        return node;
    }

    default:
        return node;
    }
}

void optimize_ast(CompilerDef* compiler, ASTNode** root) {
    if (root && *root) {
        *root = fold_node(compiler, *root);
    }
}

#include "Emitter.h"

// Helper to check if opcode is a Store Local
bool isStoreLocal(OPCode op) {
    return (op == OP_STORE32_LOCAL || op == OP_STOREp_LOCAL ||
        op == OP_STORE8_LOCAL);
}

// Helper to check if opcode is a Load Local
bool isLoadLocal(OPCode op) {
    return (op == OP_LOAD32_LOCAL || //op == OP_LOAD64_LOCAL ||
        op == OP_LOAD8_LOCAL || op == OP_LOADf_LOCAL || op == OP_LOADp_LOCAL);
}
// ============================================================================
// EMITTER HELPER FUNCTIONS
// ============================================================================

// Check if an instruction READS the register 'reg'
bool readsRegister(IREntry* ir, int reg) {
    if (ir->type != IR_INSTRUCTION || ir->op == OP_HALT) return false;

    // Ownership metadata operations address the reference register bank.
    // Conservatively keep the corresponding register live where appropriate.
    if ((ir->op == OP_MARK_UNCLAIMED_REF ||
        ir->op == OP_DESTROY_UNCLAIMED_KEEP_REF) && ir->a == reg)
        return true;

    // 1. Check A (Only for Stores and Conditional Jumps)
    // Note: STORE_K does NOT read a register (A is const index)
    if (ir->op == OP_STORE32_LOCAL || ir->op == OP_STOREp_LOCAL ||
        ir->op == OP_STORE_FIELDi || ir->op == OP_STORE_FIELDf ||
        ir->op == OP_STORE_FIELDp ||
        ir->op == OP_JUMP_IF_FALSE || ir->op == OP_JUMP_IF_TRUE) {
        if (ir->a == reg) return true;
    }

    // 2. Check B (Standard Source)
    if (ir->b == reg) return true;

    if (ir->op == OP_SQRTf || ir->op == OP_SINf || ir->op == OP_COSf || ir->op == OP_ABSf)
        return false;

    // 3. Check C (Standard Source, ONLY if not Immediate)
    // If is_AD is true, 'c' is a value, not a register.
    if (!ir->is_AD && ir->c == reg) return true;

    return false;
}

// Check if instruction WRITES to 'reg'.
// CRITICAL: Must return TRUE for ANY instruction that modifies the register.
bool writesRegister(IREntry* ir, int reg) {
    if (ir->type != IR_INSTRUCTION) return false;

    // 1. Instructions that DEFINITELY DO NOT write to registers
    switch (ir->op) {
    case OP_STORE32_LOCAL: case OP_STOREp_LOCAL:
    case OP_STORE32_LOCAL_K:
    case OP_STORE_FIELDi:  case OP_STORE_FIELDf:  case OP_STORE_FIELDp:
    case OP_JUMP:          case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE:
    case OP_RETURN:        case OP_HALT:
    case OP_DESTROYp_LOCAL: case OP_CLEARp_LOCAL:
    case OP_MARK_OWNED_LOCAL: case OP_MARK_UNCLAIMED_REF:
    case OP_DESTROY_UNCLAIMED: case OP_DESTROY_UNCLAIMED_KEEP_REF:
        return false;
    default: break;
    }

    if (ir->op == OP_ALLOCATE || ir->op == OP_NEWARRAY) {
        return (ir->a == reg);
    }

    // 2. Handle Calls (Clobber Return Register 0)
    // This fixes the bug where GP0 was reused across a function call.
    if (ir->op == OP_CALL || ir->op == OP_CALL_C) {
        if (reg == 0) return true;
        return (ir->a == reg);
    }

    // 3. Handle Field Loads (Clobber Dest Register)
    // This fixes the bug where OP_LOAD_FIELDp r0 was ignored.
    if (ir->op == OP_GET_FIELDp  ||
        ir->op == OP_LOAD_FIELDf || ir->op == OP_LOAD_FIELDi ||
        ir->op == OP_LOADp_GLOBAL || ir->op == OP_LOADp_LOCAL ||
        ir->op == OP_ALLOCATE || ir->op == OP_NEWARRAY) {
        return (ir->a == reg);
    }

    // 4. Default: Assume it writes to Operand A
    return (ir->a == reg);
}

// Checks if the opcode is a Binary Comparison Jump (uses A, B, and C=Offset)
// Checks if the opcode is a Binary Comparison Jump (uses A, B, and C=Offset)
bool isComparisonJump(OPCode op) {
    switch (op) {
    case OP_JEQ_LOCAL_K: case OP_JNE_LOCAL_K:
    case OP_JLT_LOCAL_K: case OP_JLE_LOCAL_K:
    case OP_JGT_LOCAL_K: case OP_JGE_LOCAL_K: return true;
        // Integer Comparisons
    case OP_JEQ: case OP_JNE:
    case OP_JLT: case OP_JLE:
    case OP_JGT: case OP_JGE:
    case OP_JEQf: case OP_JNEf:
    case OP_JLTf: case OP_JLEf:
    case OP_JGTf: case OP_JGEf: return true;
    default:
        return false;
    }
}

bool isForStepJump(OPCode op) {
    return (op >= OP_FOR_STEP_LT_I32 && op <= OP_FOR_STEP_GE_I32) ||
        op == OP_FOR_STEP1_LT_I32;
}
// Remove a node and update the pointer to the next valid node
void removeIRNode(State* state, IRNode** nodePtr) {
    IRNode* node = *nodePtr; // Dereference to get the actual node
    if (!node) return;

    IRNode* nextNode = node->next;
    IRNode* prevNode = node->prev;

    if (prevNode) {
        prevNode->next = nextNode;
    }
    else {
        state->ir_head = nextNode; // We removed the head
    }

    if (nextNode) {
        nextNode->prev = prevNode;
    }
    else {
        state->ir_tail = prevNode; // We removed the tail
    }

    // IR nodes come from State::irArena in emitNode(). They are not individual
    // heap allocations, so freeing one here corrupts the arena allocator.
    // Unlink it and let arena_destroy() reclaim the whole IR list.

    // Update the caller's pointer to the NEXT node
    *nodePtr = nextNode;
}

void optimizeIR(State* state) {
    if (!state->ir_head) return;

    IRNode* node = state->ir_head;

    while (node) {
        IREntry* currentData = &node->data;
        // A return makes both temporary registers dead. Fuse only adjacent IR
        // instructions, so no label can enter the middle of this sequence.
        IRNode* second = node->next;
        IRNode* math = second ? second->next : NULL;
        IRNode* ret = math ? math->next : NULL;
        if (ret && currentData->type == IR_INSTRUCTION &&
            second->data.type == IR_INSTRUCTION && math->data.type == IR_INSTRUCTION &&
            ret->data.type == IR_INSTRUCTION && ret->data.op == OP_RETURN &&
            currentData->a != second->data.a && math->data.a == 0 &&
            math->data.b == currentData->a && math->data.c == second->data.a &&
            currentData->c >= 0 && currentData->c <= 255 &&
            second->data.c >= 0 && second->data.c <= 255) {
            OPCode fused = OP_NOP;
            if (currentData->op == OP_LOAD32_LOCAL && second->data.op == OP_LOAD32_LOCAL) {
                if (math->data.op == OP_ADD) fused = OP_RETURN_ADD_LOCAL;
                if (math->data.op == OP_SUB) fused = OP_RETURN_SUB_LOCAL;
                if (math->data.op == OP_MUL) fused = OP_RETURN_MUL_LOCAL;
            } else if (currentData->op == OP_LOADf_LOCAL && second->data.op == OP_LOADf_LOCAL) {
                if (math->data.op == OP_ADDf) fused = OP_RETURN_ADDf_LOCAL;
                if (math->data.op == OP_SUBf) fused = OP_RETURN_SUBf_LOCAL;
                if (math->data.op == OP_MULf) fused = OP_RETURN_MULf_LOCAL;
            }
            if (fused != OP_NOP) {
                int left = currentData->c, right = second->data.c;
                currentData->op = fused;
                currentData->a = left;
                currentData->b = right;
                currentData->c = 0;
                currentData->is_AD = false;
                removeIRNode(state, &node->next);
                removeIRNode(state, &node->next);
                removeIRNode(state, &node->next);
            }
        }
        IRNode* store = node->next;
        if (store && currentData->type == IR_INSTRUCTION &&
            store->data.type == IR_INSTRUCTION && store->data.op == OP_STORE32_LOCAL &&
            store->data.a == currentData->a && store->data.c >= 0 && store->data.c <= 255) {
            if ((currentData->op == OP_LOAD32_LOCAL || currentData->op == OP_LOADf_LOCAL) &&
                currentData->c >= 0 && currentData->c <= 255) {
                currentData->op = OP_LOAD_STORE_LOCAL;
                currentData->b = currentData->c;
                currentData->c = store->data.c;
                currentData->is_AD = false;
                removeIRNode(state, &node->next);
            } else if (currentData->op == OP_LOAD_Ki &&
                currentData->c >= -128 && currentData->c <= 127) {
                currentData->op = OP_LOADKi_STORE_LOCAL;
                currentData->b = store->data.c;
                currentData->c = (uint8_t)currentData->c;
                currentData->is_AD = false;
                removeIRNode(state, &node->next);
            }
        }
        IRNode* nextNode = node->next;
        bool nodeWasRemoved = false;

        // =========================================================
        // OPTIMIZATION 1: Useless Jumps (SAFE)
        // Removes: OP_JUMP LabelX ... LabelX:
        // =========================================================
        bool isJump = currentData->type == IR_INSTRUCTION && (currentData->op == OP_JUMP ||
            currentData->op == OP_JUMP_IF_FALSE ||
            currentData->op == OP_JUMP_IF_TRUE);

        if (isJump && currentData->label_id > 0) {
            if (nextNode && nextNode->data.type == IR_LABEL_DEF &&
                nextNode->data.label_id == currentData->label_id) {

                removeIRNode(state, &node);
                nodeWasRemoved = true;
            }
        }

        // =========================================================
        // OPTIMIZATION 2: Redundant Load (SAFE-ISH)
        // Store Local X, Reg A ... Load Reg B, Local X -> Move Reg B, Reg A
        // =========================================================
        if (!nodeWasRemoved && nextNode && currentData->type == IR_INSTRUCTION &&
            nextNode->data.type == IR_INSTRUCTION) {
            IREntry* nextData = &nextNode->data;
            bool sameBank =
                (currentData->op == OP_STOREp_LOCAL && nextData->op == OP_LOADp_LOCAL) ||
                (currentData->op == OP_STORE32_LOCAL &&
                 (nextData->op == OP_LOAD32_LOCAL || nextData->op == OP_LOADf_LOCAL));
            if (sameBank) {
                // Must strictly match Register and Index
                if (currentData->a == nextData->a && currentData->c == nextData->c) {
                    removeIRNode(state, &node->next);
                }
            }
        }

        /*
           DISABLE CONSTANT PROPAGATION FOR NOW
           It is causing variable corruption (overwriting nextY with 0.0).
           Once the script works, you can debug this separately.
        */

        if (!nodeWasRemoved && node) {
            node = node->next;
        }
    }
}
