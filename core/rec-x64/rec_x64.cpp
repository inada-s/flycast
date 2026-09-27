#include "build.h"

#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X64

//#define CANONICAL_TEST

#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>
using namespace Xbyak::util;

#include "types.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_interrupts.h"

#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mem.h"
#include "x64_regalloc.h"
#include "xbyak_base.h"
#include "oslib/unwind_info.h"
#include "oslib/virtmem.h"
#include "cfg/option.h"

static void (*mainloop)();
static void (*handleException)();

static u64 jmp_rsp;

namespace MemSize {
	enum {
		S8,
		S16,
		S32,
		S64,
		Count
	};
}
namespace MemOp {
	enum {
		R,
		W,
		Count
	};
}
namespace MemType {
	enum {
		Fast,
		StoreQueue,
		Slow,
		Count
	};
}

static const void *MemHandlers[MemType::Count][MemSize::Count][MemOp::Count];
static const u8 *MemHandlerStart, *MemHandlerEnd;
static UnwindInfo unwinder;
#ifndef _WIN32
static float xmmSave[4];
#endif

static void ngen_blockcheckfail(u32 pc) {
	//printf("X64 JIT: SMC invalidation at %08X\n", pc);
	rdv_BlockCheckFail(pc);
}

static void handle_sh4_exception(Sh4Context *ctx, SH4ThrownException& ex, u32 pc)
{
	if (pc & 1)
	{
		// Delay slot
		AdjustDelaySlotException(ex);
		pc--;
	}
	Do_Exception(pc, ex.expEvn);
	ctx->cycle_counter += 4;	// probably more is needed
	handleException();
}

static void interpreter_fallback(Sh4Context *ctx, u16 op, OpCallFP *oph, u32 pc)
{
	try {
		oph(ctx, op);
	} catch (SH4ThrownException& ex) {
		handle_sh4_exception(ctx, ex, pc);
	}
}

static void do_sqw_mmu_no_ex(u32 addr, Sh4Context *ctx, u32 pc)
{
	try {
		ctx->doSqWrite(addr, ctx);
	} catch (SH4ThrownException& ex) {
		handle_sh4_exception(ctx, ex, pc);
	}
}

const std::array<Xbyak::Reg32, 4> call_regs
#ifdef _WIN32
	{ ecx, edx, r8d, r9d };
#else
	{ edi, esi, edx, ecx };
#endif
const std::array<Xbyak::Reg64, 4> call_regs64
#ifdef _WIN32
	{ rcx, rdx, r8, r9 };
#else
	{ rdi, rsi, rdx, rcx };
#endif
const std::array<Xbyak::Xmm, 4> call_regsxmm { xmm0, xmm1, xmm2, xmm3 };

#ifdef _WIN32
constexpr u32 STACK_ALIGN = 0x28;	// 32-byte shadow space + 8 byte alignment
#else
constexpr u32 STACK_ALIGN = 8;
#endif

// Block linking: a block that ends in a static branch keeps the host code of its branch target (and of its
// fall-through for conditional branches) and jumps there directly while cycles remain, instead of returning
// to the main loop. The links are made lazily on the first exit and dropped (Relink) when the target block
// is discarded.
struct X64BlockInfo : RuntimeBlockInfo
{
	// 0: BranchBlock, 1: NextBlock. nullptr = not linked yet, NoLink = never link
	void *linkCode[2] {};
	static inline void * const NoLink = (void *)1;

	u32 Relink() override
	{
		if (pBranchBlock == nullptr && linkCode[0] != NoLink)
			linkCode[0] = nullptr;
		if (pNextBlock == nullptr && linkCode[1] != NoLink)
			linkCode[1] = nullptr;
		return 0;
	}
};

// Returns the host code to continue with, or nullptr to go back to the main loop
static void * DYNACALL x64LinkBlock(X64BlockInfo *block, u32 which)
{
	if (mmu_enabled() || !Sh4cntx.CpuRunning)
		return nullptr;
	const u32 target = which == 0 ? block->BranchBlock : block->NextBlock;
	if (!bm_gdxsvLinkable(target))
	{
		block->linkCode[which] = X64BlockInfo::NoLink;
		return nullptr;
	}
	// Only link to a block that is already compiled; the main loop compiles it otherwise
	RuntimeBlockInfoPtr next = bm_GetBlock(target);
	if (!next || next->addr != (target & 0x1FFFFFFF) || next->temp_block)
		return nullptr;
	RuntimeBlockInfoPtr self = bm_GetBlock((void *)block->code);
	if (!self || self.get() != block || block->temp_block)
	{
		block->linkCode[which] = X64BlockInfo::NoLink;
		return nullptr;
	}
	if (which == 0)
		block->pBranchBlock = next.get();
	else
		block->pNextBlock = next.get();
	next->AddRef(self);
	block->linkCode[which] = (void *)next->code;
	return block->linkCode[which];
}

class BlockCompiler : public BaseXbyakRec<BlockCompiler, true>
{
public:
	using BaseCompiler = BaseXbyakRec<BlockCompiler, true>;
	friend class BaseXbyakRec<BlockCompiler, true>;

	BlockCompiler(Sh4Context& sh4ctx, Sh4CodeBuffer& codeBuffer) : BaseCompiler(sh4ctx, codeBuffer), regalloc(this) { }
	BlockCompiler(Sh4Context& sh4ctx, Sh4CodeBuffer& codeBuffer, u8 *code_ptr) : BaseCompiler(sh4ctx, codeBuffer, code_ptr), regalloc(this) { }

	void compile(RuntimeBlockInfo* block, bool force_checks, bool optimise)
	{
		//printf("X86_64 compiling %08x to %p\n", block->addr, codeBuffer.get());
		current_opid = -1;

		CheckBlock(force_checks, block);

		sub(rsp, STACK_ALIGN);

		if (mmu_enabled() && block->has_fpu_op)
		{
			Xbyak::Label fpu_enabled;
			mov(rax, (uintptr_t)&sh4ctx.sr.status);
			test(dword[rax], 0x8000);			// test SR.FD bit
			jz(fpu_enabled);
			mov(call_regs[0], block->vaddr);	// pc
			mov(call_regs[1], Sh4Ex_FpuDisabled);// exception code
			GenCall((void (*)())Do_Exception);
			jmp(exit_block, T_NEAR);
			L(fpu_enabled);
		}
		mov(rax, (uintptr_t)&sh4ctx.cycle_counter);
		sub(dword[rax], block->guest_cycles);

		regalloc.DoAlloc(block);

		for (current_opid = 0; current_opid < block->oplist.size(); current_opid++)
		{
			shil_opcode& op  = block->oplist[current_opid];

			regalloc.OpBegin(&op, current_opid);

			switch (op.op)
			{
			case shop_ifb:
				if (mmu_enabled())
				{
					mov(call_regs64[2], reinterpret_cast<uintptr_t>(*OpDesc[op.rs3._imm]->oph));	// op handler
					mov(call_regs[3], block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc
				}

				if (op.rs1._imm)
				{
					mov(rax, (size_t)&sh4ctx.pc);
					mov(dword[rax], op.rs2._imm);
				}

				mov(call_regs[1], op.rs3._imm);
				mov(call_regs64[0], (uintptr_t)&sh4ctx);

				if (!mmu_enabled())
					GenCall(OpDesc[op.rs3._imm]->oph);
				else
					GenCall(interpreter_fallback);

				break;

			case shop_mov64:
			{
				verify(op.rd.is_r64f());
				verify(op.rs1.is_r64f());

#if ALLOC_F64 == false
				mov(rax, (uintptr_t)op.rs1.reg_ptr(sh4ctx));
				mov(rax, qword[rax]);
				mov(rcx, (uintptr_t)op.rd.reg_ptr(sh4ctx));
				mov(qword[rcx], rax);
#else
				Xbyak::Xmm rd0 = regalloc.MapXRegister(op.rd, 0);
				Xbyak::Xmm rs0 = regalloc.MapXRegister(op.rs1, 0);
				Xbyak::Xmm rd1 = regalloc.MapXRegister(op.rd, 1);
				Xbyak::Xmm rs1 = regalloc.MapXRegister(op.rs1, 1);
				if (rd0 == rs1)
				{
					movss(xmm0, rd0);
					movss(rd0, rs0);
					movss(rd1, xmm0);
				}
				else
				{
					if (rd0 != rs0)
						movss(rd0, rs0);
					if (rd1 != rs1)
						movss(rd1, rs1);
				}
#endif
			}
			break;

			case shop_readm:
				if (!GenReadMemImmediate(op, block))
				{
					// Not an immediate address
					shil_param_to_host_reg(op.rs1, call_regs[0]);
					if (!op.rs3.is_null())
					{
						if (op.rs3.is_imm())
							add(call_regs[0], op.rs3._imm);
						else if (regalloc.IsAllocg(op.rs3))
							add(call_regs[0], regalloc.MapRegister(op.rs3));
						else
						{
							mov(rax, (uintptr_t)op.rs3.reg_ptr(sh4ctx));
							add(call_regs[0], dword[rax]);
						}
					}
					genMmuLookup(block, op, 0);

					int size = op.size == 1 ? MemSize::S8 : op.size == 2 ? MemSize::S16 : op.size == 4 ? MemSize::S32 : MemSize::S64;
					GenCall((void (*)())MemHandlers[optimise ? MemType::Fast : MemType::Slow][size][MemOp::R], mmu_enabled());

#if ALLOC_F64 == false
					if (size == MemSize::S64)
					{
						mov(rcx, (uintptr_t)op.rd.reg_ptr(sh4ctx));
						mov(qword[rcx], rax);
					}
					else
#endif
					{
						mov(rcx, rax);
						host_reg_to_shil_param(op.rd, rcx);
					}
				}
				break;

			case shop_writem:
			{
				if (!GenWriteMemImmediate(op, block))
				{
					shil_param_to_host_reg(op.rs1, call_regs[0]);
					if (!op.rs3.is_null())
					{
						if (op.rs3.is_imm())
							add(call_regs[0], op.rs3._imm);
						else if (regalloc.IsAllocg(op.rs3))
							add(call_regs[0], regalloc.MapRegister(op.rs3));
						else
						{
							mov(rax, (uintptr_t)op.rs3.reg_ptr(sh4ctx));
							add(call_regs[0], dword[rax]);
						}
					}
					genMmuLookup(block, op, 1);

#if ALLOC_F64 == false
					if (op.size == 8)
					{
						mov(rax, (uintptr_t)op.rs2.reg_ptr(sh4ctx));
						mov(call_regs64[1], qword[rax]);
					}
					else
#endif
						shil_param_to_host_reg(op.rs2, call_regs64[1]);

					int size = op.size == 1 ? MemSize::S8 : op.size == 2 ? MemSize::S16 : op.size == 4 ? MemSize::S32 : MemSize::S64;
					GenCall((void (*)())MemHandlers[optimise ? MemType::Fast : MemType::Slow][size][MemOp::W], mmu_enabled());
				}
			}
			break;

			case shop_jcond:
			case shop_jdyn:
			case shop_mov32:
				genBaseOpcode(op);
				break;

#ifndef CANONICAL_TEST
			case shop_sync_sr:
				GenCall(UpdateSR);
				break;
			case shop_sync_fpscr:
				mov(call_regs64[0], (uintptr_t)&sh4ctx);
				GenCall(Sh4Context::UpdateFPSCR);
				break;

			case shop_negc:
				{
					Xbyak::Reg32 rs2;
					if (op.rs2.is_reg())
					{
						rs2 = regalloc.MapRegister(op.rs2);
						if (regalloc.mapg(op.rd) == regalloc.mapg(op.rs2))
						{
							mov(ecx, rs2);
							rs2 = ecx;
						}
					}
					Xbyak::Reg32 rd = regalloc.MapRegister(op.rd);
					if (op.rs1.is_imm())
						mov(rd, op.rs1.imm_value());
					else if (regalloc.mapg(op.rd) != regalloc.mapg(op.rs1))
						mov(rd, regalloc.MapRegister(op.rs1));
					Xbyak::Reg64 rd64 = rd.cvt64();
					neg(rd64);
					if (op.rs2.is_imm())
						sub(rd64, op.rs2.imm_value());
					else
						sub(rd64, rs2.cvt64());
					Xbyak::Reg64 rd2_64 = regalloc.MapRegister(op.rd2).cvt64();
					mov(rd2_64, rd64);
					shr(rd2_64, 63);
				}
				break;

			case shop_mul_s64:
				movsxd(rax, regalloc.MapRegister(op.rs1));
				if (op.rs2.is_reg())
					movsxd(rcx, regalloc.MapRegister(op.rs2));
				else
					mov(rcx, (s64)(s32)op.rs2._imm);
				mul(rcx);
				mov(regalloc.MapRegister(op.rd), eax);
				shr(rax, 32);
				mov(regalloc.MapRegister(op.rd2), eax);
				break;

			case shop_pref:
				{
					Xbyak::Label no_sqw;
					if (op.rs1.is_imm())
					{
						// this test shouldn't be necessary
						if ((op.rs1._imm & 0xFC000000) != 0xE0000000)
							break;

						mov(call_regs[0], op.rs1._imm);
					}
					else
					{
						Xbyak::Reg32 rn;
						if (regalloc.IsAllocg(op.rs1))
						{
							rn = regalloc.MapRegister(op.rs1);
						}
						else
						{
							mov(rax, (uintptr_t)op.rs1.reg_ptr(sh4ctx));
							mov(eax, dword[rax]);
							rn = eax;
						}
						mov(ecx, rn);
						shr(ecx, 26);
						cmp(ecx, 0x38);
						jne(no_sqw);

						mov(call_regs[0], rn);
					}
					mov(call_regs64[1], (uintptr_t)&sh4ctx);
					if (mmu_enabled())
					{
						mov(call_regs[2], block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc
						GenCall(do_sqw_mmu_no_ex);
					}
					else
					{
						mov(rax, (size_t)&sh4ctx.doSqWrite);
						saveXmmRegisters();
						call(qword[rax]);
						restoreXmmRegisters();
					}
					L(no_sqw);
				}
				break;

			case shop_frswap:
				mov(rax, (uintptr_t)op.rs1.reg_ptr(sh4ctx));
				mov(rcx, (uintptr_t)op.rd.reg_ptr(sh4ctx));
				if (cpu.has(Cpu::tAVX512F))
				{
					vmovaps(zmm0, zword[rax]);
					vmovaps(zmm1, zword[rcx]);
					vmovaps(zword[rax], zmm1);
					vmovaps(zword[rcx], zmm0);
				}
				else if (cpu.has(Cpu::tAVX))
				{
					vmovaps(ymm0, yword[rax]);
					vmovaps(ymm1, yword[rcx]);
					vmovaps(yword[rax], ymm1);
					vmovaps(yword[rcx], ymm0);

					vmovaps(ymm0, yword[rax + 32]);
					vmovaps(ymm1, yword[rcx + 32]);
					vmovaps(yword[rax + 32], ymm1);
					vmovaps(yword[rcx + 32], ymm0);
				}
				else
				{
					for (int i = 0; i < 4; i++)
					{
						movaps(xmm0, xword[rax + (i * 16)]);
						movaps(xmm1, xword[rcx + (i * 16)]);
						movaps(xword[rax + (i * 16)], xmm1);
						movaps(xword[rcx + (i * 16)], xmm0);
					}
				}
				break;

#ifndef STRICT_MODE
			case shop_ftrv:
				// Same arithmetic as the canonical innerProduct<4>: double products summed in order, then
				// rounded to float. Each double lane computes one result, so the results are bit-exact.
				mov(rax, (uintptr_t)op.rs1.reg_ptr(sh4ctx));	// fn
				mov(rcx, (uintptr_t)op.rs2.reg_ptr(sh4ctx));	// fm (xmtrx)
				for (int j = 0; j < 4; j++)
				{
					cvtss2sd(xmm4, dword[rax + j * 4]);
					unpcklpd(xmm4, xmm4);
					Xbyak::Xmm lo = j == 0 ? xmm0 : xmm2;
					Xbyak::Xmm hi = j == 0 ? xmm1 : xmm3;
					cvtps2pd(lo, qword[rcx + j * 16]);
					mulpd(lo, xmm4);
					cvtps2pd(hi, qword[rcx + j * 16 + 8]);
					mulpd(hi, xmm4);
					if (j != 0)
					{
						addpd(xmm0, xmm2);
						addpd(xmm1, xmm3);
					}
				}
				cvtpd2ps(xmm0, xmm0);
				cvtpd2ps(xmm1, xmm1);
				movlhps(xmm0, xmm1);
				mov(rax, (uintptr_t)op.rd.reg_ptr(sh4ctx));
				movups(xword[rax], xmm0);
				break;

			case shop_fipr:
				// Same arithmetic as the canonical innerProduct: ((p0 + p1) + p2) + p3 in double
				mov(rax, (uintptr_t)op.rs1.reg_ptr(sh4ctx));
				mov(rcx, (uintptr_t)op.rs2.reg_ptr(sh4ctx));
				cvtps2pd(xmm0, qword[rax]);
				cvtps2pd(xmm1, qword[rcx]);
				mulpd(xmm0, xmm1);			// p0 p1
				cvtps2pd(xmm2, qword[rax + 8]);
				cvtps2pd(xmm3, qword[rcx + 8]);
				mulpd(xmm2, xmm3);			// p2 p3
				movapd(xmm1, xmm0);
				unpckhpd(xmm1, xmm1);
				addsd(xmm0, xmm1);
				addsd(xmm0, xmm2);
				unpckhpd(xmm2, xmm2);
				addsd(xmm0, xmm2);
				cvtsd2ss(xmm0, xmm0);
				if (regalloc.IsAllocf(op.rd))
					movss(regalloc.MapXRegister(op.rd), xmm0);
				else
				{
					mov(rax, (uintptr_t)op.rd.reg_ptr(sh4ctx));
					movss(dword[rax], xmm0);
				}
				break;
#endif

			case shop_fmac:
				{
					Xbyak::Xmm rs1 = regalloc.MapXRegister(op.rs1);
					Xbyak::Xmm rs2 = regalloc.MapXRegister(op.rs2);
					Xbyak::Xmm rs3 = regalloc.MapXRegister(op.rs3);
					Xbyak::Xmm rd = regalloc.MapXRegister(op.rd);
					if (rd == rs2)
					{
						movss(xmm1, rs2);
						rs2 = xmm1;
					}
					if (rd == rs3)
					{
						movss(xmm2, rs3);
						rs3 = xmm2;
					}
					if (op.rs1.is_imm()) // FIXME MapXRegister(op.rs1) would have failed
					{
						mov(eax, op.rs1._imm);
						movd(rd, eax);
					}
					else if (rd != rs1)
					{
						movss(rd, rs1);
					}
#ifndef GDXSV_FP_COMPAT // gdxsv: fused multiply-add breaks sync of replays and netplay
					if (cpu.has(Cpu::tFMA) && !config::GGPOEnable)
						vfmadd231ss(rd, rs2, rs3);
					else
#endif
					{
						movss(xmm0, rs2);
						mulss(xmm0, rs3);
						addss(rd, xmm0);
					}
				}
				break;
#endif

			default:
#ifndef CANONICAL_TEST
				if (!genBaseOpcode(op))
#endif
					shil_chf[op.op](&op);
				break;
			}
			regalloc.OpEnd(&op);
		}
		regalloc.Cleanup();
		current_opid = -1;

		mov(rax, (size_t)&sh4ctx.pc);

		switch (block->BlockType) {

		case BET_StaticJump:
		case BET_StaticCall:
			//next_pc = block->BranchBlock;
			mov(dword[rax], block->BranchBlock);
			genBlockLink(block, 0);
			break;

		case BET_Cond_0:
		case BET_Cond_1:
			{
				//next_pc = next_pc_value;
				//if (*jdyn == 0)
				//next_pc = branch_pc_value;

				mov(dword[rax], block->NextBlock);

				if (block->has_jcond)
					mov(rdx, (size_t)&sh4ctx.jdyn);
				else
					mov(rdx, (size_t)&sh4ctx.sr.T);

				cmp(dword[rdx], block->BlockType & 1);
				Xbyak::Label branch_not_taken;

				jne(branch_not_taken, T_NEAR);
				mov(dword[rax], block->BranchBlock);
				genBlockLink(block, 0);
				jmp(exit_block, T_NEAR);
				L(branch_not_taken);
				genBlockLink(block, 1);
			}
			break;

		case BET_DynamicJump:
		case BET_DynamicCall:
		case BET_DynamicRet:
			//next_pc = *jdyn;
			mov(rdx, (size_t)&sh4ctx.jdyn);
			mov(edx, dword[rdx]);
			mov(dword[rax], edx);
			genDynamicLink(block);
			break;

		case BET_DynamicIntr:
		case BET_StaticIntr:
			if (block->BlockType == BET_DynamicIntr) {
				//next_pc = *jdyn;
				mov(rdx, (size_t)&sh4ctx.jdyn);
				mov(edx, dword[rdx]);
				mov(dword[rax], edx);
			}
			else {
				//next_pc = next_pc_value;
				mov(dword[rax], block->NextBlock);
			}

			GenCall(UpdateINTC);
			break;

		default:
			die("Invalid block end type");
		}

		L(exit_block);
		add(rsp, STACK_ALIGN);
		ret();

		ready();

		block->code = (DynarecCodeEntryPtr)getCode();
		block->host_code_size = getSize();

		codeBuffer.advance(getSize());
	}

	// Jumps to the linked block while cycles remain; falls through (to the block exit) otherwise.
	// sh4ctx.pc is already set to the target.
	void genBlockLink(RuntimeBlockInfo *block, u32 which)
	{
		if (mmu_enabled())
			return;
		X64BlockInfo *xblock = static_cast<X64BlockInfo *>(block);
		Xbyak::Label linked, unlinked, done;

		mov(rax, (uintptr_t)&xblock->linkCode[which]);
		mov(rax, qword[rax]);
		cmp(rax, 1);	// X64BlockInfo::NoLink
		je(done, T_NEAR);
		test(rax, rax);
		jz(unlinked, T_NEAR);

		L(linked);
		mov(rdx, (uintptr_t)&sh4ctx.cycle_counter);
		cmp(dword[rdx], 0);
		jle(done, T_NEAR);
		add(rsp, STACK_ALIGN);
		jmp(rax);

		L(unlinked);
		mov(call_regs64[0], (uintptr_t)xblock);
		mov(call_regs[1], which);
		GenCall(x64LinkBlock, true);
		test(rax, rax);
		jnz(linked, T_NEAR);

		L(done);
	}

	// Dynamic branch (target pc in edx): look the target up in the fpcb table and jump there while cycles remain.
	// bm_GetCodeByVAddr must still see the gdxsv render call site, its return address and, inside the render
	// call, the output-only function entries, so those go back to the main loop.
	void genDynamicLink(RuntimeBlockInfo *block)
	{
		if (mmu_enabled())
			return;
		Xbyak::Label notInRender, fast, done;

		mov(rax, (uintptr_t)bm_gdxsvRenderReturnPtr());
		cmp(dword[rax], 0);
		je(notInRender);
		// inside the render call: the target may be an output-only function entry
		mov(eax, edx);
		shr(eax, 1);
		and_(eax, 4095);
		mov(rcx, (uintptr_t)bm_gdxsvRenderOnlyMaskPtr());
		bt(qword[rcx], rax);
		jc(done, T_NEAR);
		L(notInRender);
		mov(rax, (uintptr_t)&settings.gdxsv.skipRenderingBaseAddr);
		mov(ecx, dword[rax]);
		test(ecx, ecx);
		jz(fast);
		cmp(edx, ecx);
		je(done, T_NEAR);
		add(ecx, 4);
		cmp(edx, ecx);
		je(done, T_NEAR);

		L(fast);
		mov(rax, (uintptr_t)&sh4ctx.cycle_counter);
		cmp(dword[rax], 0);
		jle(done, T_NEAR);
		mov(eax, edx);
		shr(eax, 1);
		and_(eax, FPCB_MASK);
		mov(rcx, (uintptr_t)&p_sh4rcb->fpcb[0]);
		mov(rax, qword[rcx + rax * 8]);
		add(rsp, STACK_ALIGN);
		jmp(rax);

		L(done);
	}

	void canonStart(const shil_opcode& op)
	{
		CC_pars.clear();
	}

	void canonParam(const shil_opcode& op, const shil_param *prm, CanonicalParamType tp) {
		switch (tp)
		{

		case CPT_u32:
		case CPT_ptr:
		case CPT_f32:
		case CPT_sh4ctx:
		{
			CC_PS t = { tp, prm };
			CC_pars.push_back(t);
			break;
		}

		// store from EAX
		case CPT_u64rvL:
		case CPT_u32rv:
			mov(rcx, rax);
			host_reg_to_shil_param(*prm, ecx);
			break;

		case CPT_u64rvH:
			// assuming CPT_u64rvL has just been called
			shr(rcx, 32);
			host_reg_to_shil_param(*prm, ecx);
			break;

		// store from xmm0
		case CPT_f32rv:
			host_reg_to_shil_param(*prm, xmm0);
			break;
		}
	}

	void canonCall(const shil_opcode& op, void* function)
	{
		int regused = 0;
		int xmmused = 0;

		for (int i = CC_pars.size(); i-- > 0;)
		{
			verify(xmmused < 4 && regused < 4);
			const shil_param& prm = *CC_pars[i].prm;
			switch (CC_pars[i].type) {
				//push the contents

			case CPT_u32:
				shil_param_to_host_reg(prm, call_regs[regused++]);
				break;

			case CPT_f32:
				shil_param_to_host_reg(prm, call_regsxmm[xmmused++]);
				break;

				//push the ptr itself
			case CPT_ptr:
				verify(prm.is_reg());
				mov(call_regs64[regused++], (size_t)prm.reg_ptr(sh4ctx));
				break;

			case CPT_sh4ctx:
				mov(call_regs64[regused++], (uintptr_t)&sh4ctx);
				break;

            default:
               // Other cases handled in canonParam
               break;
			}
		}
		GenCall((void (*)())function);
#if ALLOC_F64 == true
		for (const CC_PS& ccParam : CC_pars)
		{
			const shil_param& prm = *ccParam.prm;
			if (ccParam.type == CPT_ptr && prm.count() == 2 && regalloc.IsAllocf(prm) && (op.rd._reg == prm._reg || op.rd2._reg == prm._reg)) {
				// fsca rd param is a pointer to a 64-bit reg so reload the regs if allocated
				mov(rax, (size_t)GetRegPtr(sh4ctx, prm._reg));
				movss(regalloc.MapXRegister(prm, 0), dword[rax]);
				mov(rax, (size_t)GetRegPtr(sh4ctx, prm._reg + 1));
				movss(regalloc.MapXRegister(prm, 1), dword[rax]);
			}
		}
#endif
	}

	void RegPreload(u32 reg, Xbyak::Operand::Code nreg)
	{
		mov(rax, (size_t)GetRegPtr(sh4ctx, reg));
		mov(Xbyak::Reg32(nreg), dword[rax]);
	}
	void RegWriteback(u32 reg, Xbyak::Operand::Code nreg)
	{
		mov(rax, (size_t)GetRegPtr(sh4ctx, reg));
		mov(dword[rax], Xbyak::Reg32(nreg));
	}
	void RegPreload_FPU(u32 reg, s8 nreg)
	{
		mov(rax, (size_t)GetRegPtr(sh4ctx, reg));
		movss(Xbyak::Xmm(nreg), dword[rax]);
	}
	void RegWriteback_FPU(u32 reg, s8 nreg)
	{
		mov(rax, (size_t)GetRegPtr(sh4ctx, reg));
		movss(dword[rax], Xbyak::Xmm(nreg));
	}

	void genMainloop()
	{
		unwinder.start((void *)getCurr());

		push(rbx);
		unwinder.pushReg(getSize(), Xbyak::Operand::RBX);
		push(rbp);
		unwinder.pushReg(getSize(), Xbyak::Operand::RBP);
#ifdef _WIN32
		push(rdi);
		unwinder.pushReg(getSize(), Xbyak::Operand::RDI);
		push(rsi);
		unwinder.pushReg(getSize(), Xbyak::Operand::RSI);
#endif
		push(r12);
		unwinder.pushReg(getSize(), Xbyak::Operand::R12);
		push(r13);
		unwinder.pushReg(getSize(), Xbyak::Operand::R13);
		push(r14);
		unwinder.pushReg(getSize(), Xbyak::Operand::R14);
		push(r15);
		unwinder.pushReg(getSize(), Xbyak::Operand::R15);
		sub(rsp, STACK_ALIGN);
		unwinder.allocStack(getSize(), STACK_ALIGN);
		unwinder.endProlog(getSize());

		mov(qword[rip + &jmp_rsp], rsp);

	//run_loop:
		Xbyak::Label run_loop;
		L(run_loop);
		Xbyak::Label end_run_loop;
		mov(rax, (size_t)&sh4ctx.CpuRunning);
		mov(edx, dword[rax]);

		test(edx, edx);
		je(end_run_loop);

	//slice_loop:
		Xbyak::Label slice_loop;
		L(slice_loop);
		mov(rax, (size_t)&sh4ctx.pc);
		mov(call_regs[0], dword[rax]);
		call(bm_GetCodeByVAddr);
		call(rax);
		mov(rax, (uintptr_t)&sh4ctx.cycle_counter);
		mov(ecx, dword[rax]);
		test(ecx, ecx);
		jg(slice_loop);

		add(ecx, SH4_TIMESLICE);
		mov(dword[rax], ecx);
		call(UpdateSystem_INTC);
		jmp(run_loop);

	//end_run_loop:
		L(end_run_loop);
		add(rsp, STACK_ALIGN);
		pop(r15);
		pop(r14);
		pop(r13);
		pop(r12);
#ifdef _WIN32
		pop(rsi);
		pop(rdi);
#endif
		pop(rbp);
		pop(rbx);
		ret();
		size_t unwindSize = unwinder.end(getSize());
		setSize(getSize() + unwindSize);

		unwinder.start((void *)getCurr());
		size_t startOffset = getSize();
#ifdef _WIN32
		// 32-byte shadow space + 8 for stack 16-byte alignment
		unwinder.allocStack(0, 40);
#else
		// stack 16-byte alignment
		unwinder.allocStack(0, 8);
#endif
		unwinder.endProlog(0);

	//handleException:
		Xbyak::Label handleExceptionLabel;
		L(handleExceptionLabel);
		mov(rsp, qword[rip + &jmp_rsp]);
		jmp(run_loop);

		genMemHandlers();

		size_t savedSize = getSize();
		setSize(codeBuffer.getFreeSpace() - 128 - startOffset);
		unwindSize = unwinder.end(getSize());
		verify(unwindSize <= 128);
		setSize(savedSize);

		ready();
		mainloop = (void (*)())getCode();
		handleException = (void(*)())handleExceptionLabel.getAddress();

		codeBuffer.advance(getSize());
	}

	bool rewriteMemAccess(host_context_t &context)
	{
		if (!addrspace::virtmemEnabled())
			return false;

		//printf("rewriteMemAccess pc %p\n", context.pc);
		if (context.pc < (size_t)MemHandlerStart || context.pc >= (size_t)MemHandlerEnd)
			return false;

		u8 *retAddr = *(u8 **)context.rsp;
		void *ca = *(s32 *)(retAddr - 4) + retAddr;
		for (int size = 0; size < MemSize::Count; size++)
		{
			for (int op = 0; op < MemOp::Count; op++)
			{
				if ((void *)MemHandlers[MemType::Fast][size][op] != ca)
					continue;

				//found !
				const u8 *start = getCurr();
				u32 memAddress = context.r9;
				if (op == MemOp::W && size >= MemSize::S32 && (memAddress >> 26) == 0x38)
					call(MemHandlers[MemType::StoreQueue][size][MemOp::W]);
				else
					call(MemHandlers[MemType::Slow][size][op]);
				verify(getCurr() - start == 5);

				ready();

				context.pc = (uintptr_t)(retAddr - 5);
				// remove the call from the stack
				context.rsp += 8;
				//restore the addr from r9 to arg0 (rcx or rdi) so it's valid again
#ifdef _WIN32
				context.rcx = memAddress;
#else
				context.rdi = memAddress;
#endif

				return true;
			}
		}
		ERROR_LOG(DYNAREC, "rewriteMemAccess code not found: host pc %p", (void *)context.pc);
		die("Failed to match the code");

		return false;
	}

private:
	void genMmuLookup(const RuntimeBlockInfo* block, const shil_opcode& op, u32 write)
	{
		if (mmu_enabled())
		{
#ifdef FAST_MMU
			Xbyak::Label inCache;
			Xbyak::Label done;

			mov(eax, call_regs[0]);
			shr(eax, 12);
			if ((uintptr_t)mmuAddressLUT >> 32 != 0)
			{
				mov(r9, (uintptr_t)mmuAddressLUT);
				mov(eax, dword[r9 + rax * 4]);
			}
			else
			{
				mov(eax, dword[(uintptr_t)mmuAddressLUT + rax * 4]);
			}
			test(eax, eax);
			jne(inCache);
#endif
			mov(call_regs[1], write);
			mov(call_regs[2], block->vaddr + op.guest_offs - (op.delay_slot ? 2 : 0));	// pc
			GenCall(mmuDynarecLookup);
			mov(call_regs[0], eax);
#ifdef FAST_MMU
			jmp(done);
			L(inCache);
			and_(call_regs[0], 0xFFF);
			or_(call_regs[0], eax);
			L(done);
#endif
		}
	}
	bool GenReadMemImmediate(const shil_opcode& op, RuntimeBlockInfo* block)
	{
		if (!op.rs1.is_imm())
			return false;
		void *ptr;
		bool isram;
		u32 addr;
		if (!rdv_readMemImmediate(op.rs1._imm, op.size, ptr, isram, addr, block))
			return false;

		if (isram)
		{
			// Immediate pointer to RAM: super-duper fast access
			mov(rax, reinterpret_cast<uintptr_t>(ptr));
			switch (op.size)
			{
			case 1:
				if (regalloc.IsAllocg(op.rd))
					movsx(regalloc.MapRegister(op.rd), byte[rax]);
				else
				{
					movsx(eax, byte[rax]);
					mov(rcx, (uintptr_t)op.rd.reg_ptr(sh4ctx));
					mov(dword[rcx], eax);
				}
				break;

			case 2:
				if (regalloc.IsAllocg(op.rd))
					movsx(regalloc.MapRegister(op.rd), word[rax]);
				else
				{
					movsx(eax, word[rax]);
					mov(rcx, (uintptr_t)op.rd.reg_ptr(sh4ctx));
					mov(dword[rcx], eax);
				}
				break;

			case 4:
				if (regalloc.IsAllocg(op.rd))
					mov(regalloc.MapRegister(op.rd), dword[rax]);
				else if (regalloc.IsAllocf(op.rd))
					movd(regalloc.MapXRegister(op.rd), dword[rax]);
				else
				{
					mov(eax, dword[rax]);
					mov(rcx, (uintptr_t)op.rd.reg_ptr(sh4ctx));
					mov(dword[rcx], eax);
				}
				break;

			case 8:
#if ALLOC_F64 == false
				mov(rcx, qword[rax]);
				mov(rax, (uintptr_t)op.rd.reg_ptr(sh4ctx));
				mov(qword[rax], rcx);
#else
				movd(regalloc.MapXRegister(op.rd, 0), dword[rax]);
				movd(regalloc.MapXRegister(op.rd, 1), dword[rax + 4]);
#endif
				break;

			default:
				die("Invalid immediate size");
					break;
			}
		}
		else
		{
			// Not RAM: the returned pointer is a memory handler
			if (op.size == 8)
			{
				// Need to call the handler twice
				mov(call_regs[0], addr);
				GenCall((void (*)())ptr);
#if ALLOC_F64 == false
				mov(rcx, (size_t)op.rd.reg_ptr(sh4ctx));
				mov(dword[rcx], eax);
#else
				movd(regalloc.MapXRegister(op.rd, 0), eax);
#endif

				mov(call_regs[0], addr + 4);
				GenCall((void (*)())ptr);
#if ALLOC_F64 == false
				mov(rcx, (size_t)op.rd.reg_ptr(sh4ctx) + 4);
				mov(dword[rcx], eax);
#else
				movd(regalloc.MapXRegister(op.rd, 1), eax);
#endif
			}
			else
			{
				mov(call_regs[0], addr);

				switch(op.size)
				{
				case 1:
					GenCall((void (*)())ptr);
					movsx(eax, al);
					break;

				case 2:
					GenCall((void (*)())ptr);
					movsx(eax, ax);
					break;

				case 4:
					GenCall((void (*)())ptr);
					break;

				default:
					die("Invalid immediate size");
						break;
				}
				mov(ecx, eax);
				host_reg_to_shil_param(op.rd, ecx);
			}
		}

		return true;
	}

	bool GenWriteMemImmediate(const shil_opcode& op, RuntimeBlockInfo* block)
	{
		if (!op.rs1.is_imm())
			return false;
		void *ptr;
		bool isram;
		u32 addr;
		if (!rdv_writeMemImmediate(op.rs1._imm, op.size, ptr, isram, addr, block))
			return false;
		if (isram)
		{
			// Immediate pointer to RAM: super-duper fast access
			mov(rax, reinterpret_cast<uintptr_t>(ptr));
			switch (op.size)
			{
			case 1:
				if (regalloc.IsAllocg(op.rs2))
					mov(byte[rax], regalloc.MapRegister(op.rs2).cvt8());
				else if (op.rs2.is_imm())
					mov(byte[rax], (u8)op.rs2._imm);
				else
				{
					mov(rcx, (uintptr_t)op.rs2.reg_ptr(sh4ctx));
					mov(cl, byte[rcx]);
					mov(byte[rax], cl);
				}
				break;

			case 2:
				if (regalloc.IsAllocg(op.rs2))
					mov(word[rax], regalloc.MapRegister(op.rs2).cvt16());
				else if (op.rs2.is_imm())
					mov(word[rax], (u16)op.rs2._imm);
				else
				{
					mov(rcx, (uintptr_t)op.rs2.reg_ptr(sh4ctx));
					mov(cx, word[rcx]);
					mov(word[rax], cx);
				}
				break;

			case 4:
				if (regalloc.IsAllocg(op.rs2))
					mov(dword[rax], regalloc.MapRegister(op.rs2));
				else if (regalloc.IsAllocf(op.rs2))
					movd(dword[rax], regalloc.MapXRegister(op.rs2));
				else if (op.rs2.is_imm())
					mov(dword[rax], op.rs2._imm);
				else
				{
					mov(rcx, (uintptr_t)op.rs2.reg_ptr(sh4ctx));
					mov(ecx, dword[rcx]);
					mov(dword[rax], ecx);
				}
				break;

			case 8:
#if ALLOC_F64 == false
				mov(rcx, (uintptr_t)op.rs2.reg_ptr(sh4ctx));
				mov(rcx, qword[rcx]);
				mov(qword[rax], rcx);
#else
				movd(dword[rax], regalloc.MapXRegister(op.rs2, 0));
				movd(dword[rax + 4], regalloc.MapXRegister(op.rs2, 1));
#endif
				break;

			default:
				die("Invalid immediate size");
				break;
			}
		}
		else
		{
			// Not RAM: the returned pointer is a memory handler
			mov(call_regs[0], addr);
			shil_param_to_host_reg(op.rs2, call_regs[1]);

			GenCall((void (*)())ptr);
		}

		return true;
	}

	void CheckBlock(bool force_checks, RuntimeBlockInfo* block)
	{
		if (mmu_enabled() || force_checks)
			mov(call_regs[0], block->addr);

		// FIXME This test shouldn't be necessary
		// However the decoder makes various assumptions about the current PC value, which are simply not
		// true in a virtualized memory model. So this can only work if virtual and phy addresses are the
		// same at compile and run times.
		if (mmu_enabled())
		{
			mov(rax, (uintptr_t)&sh4ctx.pc);
			cmp(dword[rax], block->vaddr);
			jne(reinterpret_cast<const void*>(&ngen_blockcheckfail));
		}

		if (!force_checks)
			return;

		s32 sz=block->sh4_code_size;
		u32 sa=block->addr;

		void* ptr = (void*)GetMemPtr(sa, sz > 8 ? 8 : sz);
		if (ptr)
		{
			while (sz > 0)
			{
				uintptr_t uintptr = reinterpret_cast<uintptr_t>(ptr);
				mov(rax, uintptr);

				if (sz >= 8 && !(uintptr & 7)) {
					mov(rdx, *(u64*)ptr);
					cmp(qword[rax], rdx);
					sz -= 8;
					sa += 8;
				}
				else if (sz >= 4 && !(uintptr & 3)) {
					mov(edx, *(u32*)ptr);
					cmp(dword[rax], edx);
					sz -= 4;
					sa += 4;
				}
				else {
					mov(edx, *(u16*)ptr);
					cmp(word[rax],dx);
					sz -= 2;
					sa += 2;
				}
				jne(reinterpret_cast<const void*>(CC_RX2RW(&ngen_blockcheckfail)));
				ptr = (void*)GetMemPtr(sa, sz > 8 ? 8 : sz);
			}
		}
	}

	void genMemHandlers()
	{
		// make sure the memory handlers are set
		verify(ReadMem8 != nullptr);

		MemHandlerStart = getCurr();
		for (int type = 0; type < MemType::Count; type++)
		{
			for (int size = 0; size < MemSize::Count; size++)
			{
				for (int op = 0; op < MemOp::Count; op++)
				{
					MemHandlers[type][size][op] = getCurr();
					if (type == MemType::Fast && addrspace::virtmemEnabled())
					{
						mov(rax, (uintptr_t)addrspace::ram_base);
						mov(r9, call_regs64[0]);
						and_(call_regs[0], 0x1FFFFFFF);

						switch (size)
						{
						case MemSize::S8:
							if (op == MemOp::R)
								movsx(eax, byte[rax + call_regs64[0]]);
							else
								mov(byte[rax + call_regs64[0]], call_regs[1].cvt8());
							break;

						case MemSize::S16:
							if (op == MemOp::R)
								movsx(eax, word[rax + call_regs64[0]]);
							else
								mov(word[rax + call_regs64[0]], call_regs[1].cvt16());
							break;

						case MemSize::S32:
							if (op == MemOp::R)
								mov(eax, dword[rax + call_regs64[0]]);
							else
								mov(dword[rax + call_regs64[0]], call_regs[1]);
							break;

						case MemSize::S64:
							if (op == MemOp::R)
								mov(rax, qword[rax + call_regs64[0]]);
							else
								mov(qword[rax + call_regs64[0]], call_regs64[1]);
							break;
						}
					}
					else if (type == MemType::StoreQueue)
					{
						if (op != MemOp::W || size < MemSize::S32)
							continue;
						Xbyak::Label no_sqw;

						mov(r9d, call_regs[0]);
						shr(r9d, 26);
						cmp(r9d, 0x38);
						jne(no_sqw);
						mov(rax, (uintptr_t)sh4ctx.sq_buffer);
						and_(call_regs[0], 0x3F);

						if (size == MemSize::S32)
							mov(dword[rax + call_regs64[0]], call_regs[1]);
						else
							mov(qword[rax + call_regs64[0]], call_regs64[1]);
						ret();
						L(no_sqw);
						if (size == MemSize::S32)
							jmp((const void *)addrspace::write32);	// tail call
						else
							jmp((const void *)addrspace::write64);	// tail call
						continue;
					}
					else
					{
						// Slow path
						if (op == MemOp::R)
						{
							switch (size) {
							case MemSize::S8:
								sub(rsp, STACK_ALIGN);
								call((const void *)addrspace::read8);
								movsx(eax, al);
								add(rsp, STACK_ALIGN);
								break;
							case MemSize::S16:
								sub(rsp, STACK_ALIGN);
								call((const void *)addrspace::read16);
								movsx(eax, ax);
								add(rsp, STACK_ALIGN);
								break;
							case MemSize::S32:
								jmp((const void *)addrspace::read32);	// tail call
								continue;
							case MemSize::S64:
								jmp((const void *)addrspace::read64);	// tail call
								continue;
							default:
								die("1..8 bytes");
							}
						}
						else
						{
							switch (size) {
							case MemSize::S8:
								jmp((const void *)addrspace::write8);		// tail call
								continue;
							case MemSize::S16:
								jmp((const void *)addrspace::write16);	// tail call
								continue;
							case MemSize::S32:
								jmp((const void *)addrspace::write32);	// tail call
								continue;
							case MemSize::S64:
								jmp((const void *)addrspace::write64);	// tail call
								continue;
							default:
								die("1..8 bytes");
							}
						}
					}
					ret();
				}
			}
		}
		MemHandlerEnd = getCurr();
	}

	void saveXmmRegisters()
	{
#ifndef _WIN32
		if (current_opid == (size_t)-1)
			return;

		if (regalloc.IsMapped(xmm8, current_opid))
			movd(ptr[rip + &xmmSave[0]], xmm8);
		if (regalloc.IsMapped(xmm9, current_opid))
			movd(ptr[rip + &xmmSave[1]], xmm9);
		if (regalloc.IsMapped(xmm10, current_opid))
			movd(ptr[rip + &xmmSave[2]], xmm10);
		if (regalloc.IsMapped(xmm11, current_opid))
			movd(ptr[rip + &xmmSave[3]], xmm11);
#endif
	}

	void restoreXmmRegisters()
	{
#ifndef _WIN32
		if (current_opid == (size_t)-1)
			return;

		if (regalloc.IsMapped(xmm8, current_opid))
			movd(xmm8, ptr[rip + &xmmSave[0]]);
		if (regalloc.IsMapped(xmm9, current_opid))
			movd(xmm9, ptr[rip + &xmmSave[1]]);
		if (regalloc.IsMapped(xmm10, current_opid))
			movd(xmm10, ptr[rip + &xmmSave[2]]);
		if (regalloc.IsMapped(xmm11, current_opid))
			movd(xmm11, ptr[rip + &xmmSave[3]]);
#endif
	}

	template<class Ret, class... Params>
	void GenCall(Ret(*function)(Params...), bool skip_floats = false)
	{
		if (!skip_floats)
			saveXmmRegisters();
		call(CC_RX2RW(function));
		if (!skip_floats)
			restoreXmmRegisters();
	}

	struct CC_PS
	{
		CanonicalParamType type;
		const shil_param* prm;
	};
	std::vector<CC_PS> CC_pars;

	X64RegAlloc regalloc;
	// cpuid is slow: probe the host once, not for every compiled block
	static const Xbyak::util::Cpu& hostCpu() { static const Xbyak::util::Cpu c; return c; }
	const Xbyak::util::Cpu& cpu = hostCpu();
	size_t current_opid;
	Xbyak::Label exit_block;
};

void X64RegAlloc::Preload(u32 reg, Xbyak::Operand::Code nreg)
{
	compiler->RegPreload(reg, nreg);
}
void X64RegAlloc::Writeback(u32 reg, Xbyak::Operand::Code nreg)
{
	compiler->RegWriteback(reg, nreg);
}
void X64RegAlloc::Preload_FPU(u32 reg, s8 nreg)
{
	compiler->RegPreload_FPU(reg, nreg);
}
void X64RegAlloc::Writeback_FPU(u32 reg, s8 nreg)
{
	compiler->RegWriteback_FPU(reg, nreg);
}

class X64Dynarec : public Sh4Dynarec
{
public:
	X64Dynarec() {
		sh4Dynarec = this;
	}

	void compile(RuntimeBlockInfo* block, bool smc_checks, bool optimise) override
	{
		void* protStart = codeBuffer->get();
		size_t protSize = codeBuffer->getFreeSpace();
		virtmem::jit_set_exec(protStart, protSize, false);

		ccCompiler = new BlockCompiler(*sh4ctx, *codeBuffer);
		try {
			ccCompiler->compile(block, smc_checks, optimise);
		} catch (const Xbyak::Error& e) {
			ERROR_LOG(DYNAREC, "Fatal xbyak error: %s", e.what());
		}
		delete ccCompiler;
		ccCompiler = nullptr;
		virtmem::jit_set_exec(protStart, protSize, true);
	}

	RuntimeBlockInfo *allocateBlock() override {
		return new X64BlockInfo();
	}

	void init(Sh4Context& sh4ctx, Sh4CodeBuffer& codeBuffer) override
	{
		this->sh4ctx = &sh4ctx;
		this->codeBuffer = &codeBuffer;
	}

	void mainloop(void *) override
	{
		verify(::mainloop != nullptr);
		try {
			::mainloop();
		} catch (const SH4ThrownException& ex) {
			ERROR_LOG(DYNAREC, "SH4ThrownException in mainloop code %x", ex.expEvn);
			throw FlycastException("Fatal: Unhandled SH4 exception");
		}
	}

	void canonStart(const shil_opcode* op) override {
		ccCompiler->canonStart(*op);
	}

	void canonParam(const shil_opcode* op, const shil_param* par, CanonicalParamType tp) override {
		ccCompiler->canonParam(*op, par, tp);
	}

	void canonCall(const shil_opcode* op, void* function) override {
		ccCompiler->canonCall(*op, function);
	}

	void canonFinish(const shil_opcode* op) override {
	}

	bool rewrite(host_context_t &context, void *faultAddress) override
	{
		if (codeBuffer == nullptr)
			// init() not called yet
			return false;
		u8 *retAddr = *(u8**)context.rsp - 5;
		if (retAddr < (u8*)codeBuffer->getBase() || retAddr >= (u8*)codeBuffer->getBase() + codeBuffer->getSize())
			return false;
		virtmem::jit_set_exec(retAddr, 16, false);

		BlockCompiler compiler(*sh4ctx, *codeBuffer, retAddr);
		bool rc = false;
		try {
			rc = compiler.rewriteMemAccess(context);
		} catch (const Xbyak::Error& e) {
			ERROR_LOG(DYNAREC, "Fatal xbyak error: %s", e.what());
		}
		virtmem::jit_set_exec(retAddr, 16, true);
		return rc;
	}

	void handleException(host_context_t &context) override
	{
		context.pc = (uintptr_t)::handleException;
	}

	void reset() override
	{
		unwinder.clear();
		// Avoid generating the main loop more than once
		if (::mainloop != nullptr && ::mainloop != codeBuffer->get())
			return;

		void* protStart = codeBuffer->get();
		size_t protSize = codeBuffer->getFreeSpace();
		virtmem::jit_set_exec(protStart, protSize, false);

		BlockCompiler compiler(*sh4ctx, *codeBuffer);
		try {
			compiler.genMainloop();
		} catch (const Xbyak::Error& e) {
			ERROR_LOG(DYNAREC, "Fatal xbyak error: %s", e.what());
		}
		virtmem::jit_set_exec(protStart, protSize, true);
	}

private:
	Sh4Context *sh4ctx = nullptr;
	Sh4CodeBuffer *codeBuffer = nullptr;
	BlockCompiler *ccCompiler = nullptr;
};

static X64Dynarec instance;

#endif
