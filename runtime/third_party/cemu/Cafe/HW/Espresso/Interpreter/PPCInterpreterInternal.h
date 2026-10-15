// shim: the instruction field macros of Cemu's PPCInterpreterInternal.h (MPL-2.0) used by the PPC assembler
#pragma once
#define PPC_OPC_RC		1
#define PPC_OPC_OE		(1<<10)
#define PPC_OPC_LK		1
#define PPC_OPC_AA		(1<<1)

#define PPC_OPC_TEMPL_A(opc, rD, rA, rB, rC) {rD=((opc)>>21)&0x1f;rA=((opc)>>16)&0x1f;rB=((opc)>>11)&0x1f;rC=((opc)>>6)&0x1f;}
#define PPC_OPC_TEMPL_B(opc, BO, BI, BD) {BO=((opc)>>21)&0x1f;BI=((opc)>>16)&0x1f;BD=(uint32)(sint32)(sint16)((opc)&0xfffc);}
#define PPC_OPC_TEMPL_D_SImm(opc, rD, rA, imm) {rD=((opc)>>21)&0x1f;rA=((opc)>>16)&0x1f;imm=(uint32)(sint32)(sint16)((opc)&0xffff);}
#define PPC_OPC_TEMPL_D_UImm(opc, rD, rA, imm) {rD=((opc)>>21)&0x1f;rA=((opc)>>16)&0x1f;imm=(opc)&0xffff;}
#define PPC_OPC_TEMPL_D_Shift16(opc, rD, rA, imm) {rD=((opc)>>21)&0x1f;rA=((opc)>>16)&0x1f;imm=(opc)<<16;}
#define PPC_OPC_TEMPL_I(opc, LI) {LI=(opc)&0x3fffffc;if (LI&0x02000000) LI |= 0xfc000000;}
#define PPC_OPC_TEMPL_M(opc, rS, rA, SH, MB, ME) {rS=((opc)>>21)&0x1f;rA=((opc)>>16)&0x1f;SH=((opc)>>11)&0x1f;MB=((opc)>>6)&0x1f;ME=((opc)>>1)&0x1f;}
#define PPC_OPC_TEMPL_X(opc, rS, rA, rB) {rS=((opc)>>21)&0x1f;rA=((opc)>>16)&0x1f;rB=((opc)>>11)&0x1f;}
#define PPC_OPC_TEMPL_XFX(opc, rS, CRM) {rS=((opc)>>21)&0x1f;CRM=((opc)>>12)&0xff;}
#define PPC_OPC_TEMPL_XO(opc, rS, rA, rB) {rS=((opc)>>21)&0x1f;rA=((opc)>>16)&0x1f;rB=((opc)>>11)&0x1f;}
#define PPC_OPC_TEMPL_XL(opc, BO, BI, BD) {BO=((opc)>>21)&0x1f;BI=((opc)>>16)&0x1f;BD=((opc)>>11)&0x1f;}
#define PPC_OPC_TEMPL_XFL(opc, rB, FM) {rB=((opc)>>11)&0x1f;FM=((opc)>>17)&0xff;}

#define PPC_OPC_TEMPL3_XO() sint32 rD, rA, rB; rD=((opcode)>>21)&0x1f;rA=((opcode)>>16)&0x1f;rB=((opcode)>>11)&0x1f
#define PPC_OPC_TEMPL_X_CR() sint32 crD, crA, crB; crD=((opcode)>>21)&0x1f;crA=((opcode)>>16)&0x1f;crB=((opcode)>>11)&0x1f
