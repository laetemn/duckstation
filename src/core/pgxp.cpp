/***************************************************************************
 *   Original copyright notice from PGXP code from Beetle PSX.             *
 *   Copyright (C) 2016 by iCatButler                                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#include "pgxp.h"

namespace PGXP {
// pgxp_types.h
typedef struct PGXP_value_Tag
{
  float x;
  float y;
  float z;
  union
  {
    unsigned int flags;
    unsigned char compFlags[4];
    unsigned short halfFlags[2];
  };
  unsigned int count;
  unsigned int value;

  unsigned short gFlags;
  unsigned char lFlags;
  unsigned char hFlags;
} PGXP_value;

// pgxp_value.h
typedef union
{
  struct
  {
    u8 l, h, h2, h3;
  } b;
  struct
  {
    u16 l, h;
  } w;
  struct
  {
    s8 l, h, h2, h3;
  } sb;
  struct
  {
    s16 l, h;
  } sw;
  u32 d;
  s32 sd;
} psx_value;

typedef enum
{
  UNINITIALISED = 0,
  INVALID_PSX_VALUE = 1,
  INVALID_ADDRESS = 2,
  INVALID_BITWISE_OP = 3,
  DIVIDE_BY_ZERO = 4,
  INVALID_8BIT_LOAD = 5,
  INVALID_8BIT_STORE = 6
} PGXP_error_states;

#define NONE 0
#define ALL 0xFFFFFFFF
#define VALID 1
#define VALID_0 (VALID << 0)
#define VALID_1 (VALID << 8)
#define VALID_2 (VALID << 16)
#define VALID_3 (VALID << 24)
#define VALID_01 (VALID_0 | VALID_1)
#define VALID_012 (VALID_0 | VALID_1 | VALID_2)
#define VALID_ALL (VALID_0 | VALID_1 | VALID_2 | VALID_3)
#define INV_VALID_ALL (ALL ^ VALID_ALL)

static const PGXP_value PGXP_value_invalid_address = {0.f, 0.f, 0.f, {0}, 0, 0, INVALID_ADDRESS, 0, 0};
static const PGXP_value PGXP_value_zero = {0.f, 0.f, 0.f, {0}, 0, VALID_ALL, 0, 0, 0};

static void SetValue(PGXP_value* pV, u32 psxV);
static void MakeValid(PGXP_value* pV, u32 psxV);
static void Validate(PGXP_value* pV, u32 psxV);
static void MaskValidate(PGXP_value* pV, u32 psxV, u32 mask, u32 validMask);
static u32 ValueToTolerance(PGXP_value* pV, u32 psxV, float tolerance);

static double f16Sign(double in);
static double f16Unsign(double in);
static double fu16Trunc(double in);
static double f16Overflow(double in);

typedef union
{
  struct
  {
    s16 x;
    s16 y;
  };
  struct
  {
    u16 ux;
    u16 uy;
  };
  u32 word;
} low_value;

// pgxp_mem.h
static char* PGXP_GetMem(); // return pointer to precision memory
static u32 PGXP_ConvertAddress(u32 addr);

static PGXP_value* GetPtr(u32 addr);
static PGXP_value* ReadMem(u32 addr);

static void ValidateAndCopyMem(PGXP_value* dest, u32 addr, u32 value);
static void ValidateAndCopyMem16(PGXP_value* dest, u32 addr, u32 value, int sign);

static void WriteMem(PGXP_value* value, u32 addr);
static void WriteMem16(PGXP_value* src, u32 addr);

static void PGXP_SetLastDMA(u32 addr);
static u32 PGXP_GetLastDMA();

// pgxp_gpu.h
void PGXP_CacheVertex(short sx, short sy, const PGXP_value* _pVertex);

// pgxp_cpu.h
static PGXP_value CPU_reg_mem[34];
#define CPU_Hi CPU_reg[33]
#define CPU_Lo CPU_reg[34]
static PGXP_value CP0_reg_mem[32];

static PGXP_value* CPU_reg = CPU_reg_mem;
static PGXP_value* CP0_reg = CP0_reg_mem;

// pgxp_value.c
void SetValue(PGXP_value* pV, u32 psxV)
{
  psx_value psx;
  psx.d = psxV;

  pV->x = psx.sw.l;
  pV->y = psx.sw.h;
  pV->z = 0.f;
  pV->flags = VALID_01;
  pV->value = psx.d;
}

void MakeValid(PGXP_value* pV, u32 psxV)
{
  psx_value psx;
  psx.d = psxV;
  if (VALID_01 != (pV->flags & VALID_01))
  {
    pV->x = psx.sw.l;
    pV->y = psx.sw.h;
    pV->z = 0.f;
    pV->flags |= VALID_01;
    pV->value = psx.d;
  }
}

void Validate(PGXP_value* pV, u32 psxV)
{
  // assume pV is not NULL
  pV->flags &= (pV->value == psxV) ? ALL : INV_VALID_ALL;
}

void MaskValidate(PGXP_value* pV, u32 psxV, u32 mask, u32 validMask)
{
  // assume pV is not NULL
  pV->flags &= ((pV->value & mask) == (psxV & mask)) ? ALL : (ALL ^ (validMask));
}

u32 ValueToTolerance(PGXP_value* pV, u32 psxV, float tolerance)
{
  psx_value psx;
  psx.d = psxV;
  u32 retFlags = VALID_ALL;

  if (fabs(pV->x - psx.sw.l) >= tolerance)
    retFlags = retFlags & (VALID_1 | VALID_2 | VALID_3);

  if (fabs(pV->y - psx.sw.h) >= tolerance)
    retFlags = retFlags & (VALID_0 | VALID_2 | VALID_3);

  return retFlags;
}

/// float logical arithmetic ///

double f16Sign(double in)
{
  u32 s = in * (double)((u32)1 << 16);
  return ((double)*((s32*)&s)) / (double)((s32)1 << 16);
}
double f16Unsign(double in)
{
  return (in >= 0) ? in : ((double)in + (double)USHRT_MAX + 1);
}
double fu16Trunc(double in)
{
  u32 u = in * (double)((u32)1 << 16);
  return (double)u / (double)((u32)1 << 16);
}
double f16Overflow(double in)
{
  double out = 0;
  s64 v = ((s64)in) >> 16;
  out = v;
  return out;
}

// pgxp_mem.c
static PGXP_value Mem[3 * 2048 * 1024 / 4]; // mirror 2MB in 32-bit words * 3
static const u32 UserMemOffset = 0;
static const u32 ScratchOffset = 2048 * 1024 / 4;
static const u32 RegisterOffset = 2 * 2048 * 1024 / 4;
static const u32 InvalidAddress = 3 * 2048 * 1024 / 4;

void PGXP_InitMem()
{
  memset(Mem, 0, sizeof(Mem));
}

char* PGXP_GetMem()
{
  return (char*)(Mem); // Config.PGXP_GTE ? (char*)(Mem) : NULL;
}

/*  Playstation Memory Map (from Playstation doc by Joshua Walker)
0x0000_0000-0x0000_ffff		Kernel (64K)
0x0001_0000-0x001f_ffff		User Memory (1.9 Meg)

0x1f00_0000-0x1f00_ffff		Parallel Port (64K)

0x1f80_0000-0x1f80_03ff		Scratch Pad (1024 bytes)

0x1f80_1000-0x1f80_2fff		Hardware Registers (8K)

0x1fc0_0000-0x1fc7_ffff		BIOS (512K)

0x8000_0000-0x801f_ffff		Kernel and User Memory Mirror (2 Meg) Cached
0x9fc0_0000-0x9fc7_ffff		BIOS Mirror (512K) Cached

0xa000_0000-0xa01f_ffff		Kernel and User Memory Mirror (2 Meg) Uncached
0xbfc0_0000-0xbfc7_ffff		BIOS Mirror (512K) Uncached
*/
void ValidateAddress(u32 addr)
{
  int* pi = NULL;

  if ((addr >= 0x00000000) && (addr <= 0x007fffff))
  {
  } // Kernel + User Memory x 8
  else if ((addr >= 0x1f000000) && (addr <= 0x1f00ffff))
  {
  } // Parallel Port
  else if ((addr >= 0x1f800000) && (addr <= 0x1f8003ff))
  {
  } // Scratch Pad
  else if ((addr >= 0x1f801000) && (addr <= 0x1f802fff))
  {
  } // Hardware Registers
  else if ((addr >= 0x1fc00000) && (addr <= 0x1fc7ffff))
  {
  } // Bios
  else if ((addr >= 0x80000000) && (addr <= 0x807fffff))
  {
  } // Kernel + User Memory x 8 Cached mirror
  else if ((addr >= 0x9fc00000) && (addr <= 0x9fc7ffff))
  {
  } // Bios Cached Mirror
  else if ((addr >= 0xa0000000) && (addr <= 0xa07fffff))
  {
  } // Kernel + User Memory x 8 Uncached mirror
  else if ((addr >= 0xbfc00000) && (addr <= 0xbfc7ffff))
  {
  } // Bios Uncached Mirror
  else if (addr == 0xfffe0130)
  {
  } // Used for cache flushing
  else
  {
    //	*pi = 5;
  }
}

u32 PGXP_ConvertAddress(u32 addr)
{
  u32 memOffs = 0;
  u32 paddr = addr;

  //	ValidateAddress(addr);

  switch (paddr >> 24)
  {
    case 0x80:
    case 0xa0:
    case 0x00:
      // RAM further mirrored over 8MB
      paddr = ((paddr & 0x7FFFFF) % 0x200000) >> 2;
      paddr = UserMemOffset + paddr;
      break;
    default:
      if ((paddr >> 20) == 0x1f8)
      {
        if (paddr >= 0x1f801000)
        {
          //	paddr = ((paddr & 0xFFFF) - 0x1000);
          //	paddr = (paddr % 0x2000) >> 2;
          paddr = ((paddr & 0xFFFF) - 0x1000) >> 2;
          paddr = RegisterOffset + paddr;
          break;
        }
        else
        {
          // paddr = ((paddr & 0xFFF) % 0x400) >> 2;
          paddr = (paddr & 0x3FF) >> 2;
          paddr = ScratchOffset + paddr;
          break;
        }
      }

      paddr = InvalidAddress;
      break;
  }

#ifdef GTE_LOG
    // GTE_LOG("PGXP_Read %x [%x] |", addr, paddr);
#endif

  return paddr;
}

PGXP_value* GetPtr(u32 addr)
{
  addr = PGXP_ConvertAddress(addr);

  if (addr != InvalidAddress)
    return &Mem[addr];
  return NULL;
}

PGXP_value* ReadMem(u32 addr)
{
  return GetPtr(addr);
}

void ValidateAndCopyMem(PGXP_value* dest, u32 addr, u32 value)
{
  PGXP_value* pMem = GetPtr(addr);
  if (pMem != NULL)
  {
    Validate(pMem, value);
    *dest = *pMem;
    return;
  }

  *dest = PGXP_value_invalid_address;
}

void ValidateAndCopyMem16(PGXP_value* dest, u32 addr, u32 value, int sign)
{
  u32 validMask = 0;
  psx_value val, mask;
  PGXP_value* pMem = GetPtr(addr);
  if (pMem != NULL)
  {
    mask.d = val.d = 0;
    // determine if high or low word
    if ((addr % 4) == 2)
    {
      val.w.h = value;
      mask.w.h = 0xFFFF;
      validMask = VALID_1;
    }
    else
    {
      val.w.l = value;
      mask.w.l = 0xFFFF;
      validMask = VALID_0;
    }

    // validate and copy whole value
    MaskValidate(pMem, val.d, mask.d, validMask);
    *dest = *pMem;

    // if high word then shift
    if ((addr % 4) == 2)
    {
      dest->x = dest->y;
      dest->lFlags = dest->hFlags;
      dest->compFlags[0] = dest->compFlags[1];
    }

    // truncate value
    dest->y = (dest->x < 0) ? -1.f * sign : 0.f; // 0.f;
    dest->hFlags = 0;
    dest->value = value;
    dest->compFlags[1] = VALID; // iCB: High word is valid, just 0
    return;
  }

  *dest = PGXP_value_invalid_address;
}

void WriteMem(PGXP_value* value, u32 addr)
{
  PGXP_value* pMem = GetPtr(addr);

  if (pMem)
    *pMem = *value;
}

void WriteMem16(PGXP_value* src, u32 addr)
{
  PGXP_value* dest = GetPtr(addr);
  psx_value* pVal = NULL;

  if (dest)
  {
    pVal = (psx_value*)&dest->value;
    // determine if high or low word
    if ((addr % 4) == 2)
    {
      dest->y = src->x;
      dest->hFlags = src->lFlags;
      dest->compFlags[1] = src->compFlags[0];
      pVal->w.h = (u16)src->value;
    }
    else
    {
      dest->x = src->x;
      dest->lFlags = src->lFlags;
      dest->compFlags[0] = src->compFlags[0];
      pVal->w.l = (u16)src->value;
    }

    // overwrite z/w if valid
    if (src->compFlags[2] == VALID)
    {
      dest->z = src->z;
      dest->compFlags[2] = src->compFlags[2];
    }

    // dest->valid = dest->valid && src->valid;
    dest->gFlags |= src->gFlags; // inherit flags from both values (?)
  }
}

u32 lastDMAAddr = 0;

void PGXP_SetLastDMA(u32 addr)
{
  lastDMAAddr = PGXP_ConvertAddress(addr);
}

u32 PGXP_GetLastDMA()
{
  return lastDMAAddr;
}

// pgxp_main.c
u32 static gMode = 0;

void PGXP_Init()
{
  PGXP_InitMem();
  PGXP_InitCPU();
  PGXP_InitGTE();
}

void PGXP_SetModes(u32 modes)
{
  gMode = modes;
}

u32 PGXP_GetModes()
{
  return gMode;
}

void PGXP_EnableModes(u32 modes)
{
  gMode |= modes;
}

void PGXP_DisableModes(u32 modes)
{
  gMode = gMode & ~modes;
}

// pgxp_gte.c

// GTE registers
static PGXP_value GTE_data_reg_mem[32];
static PGXP_value GTE_ctrl_reg_mem[32];

static PGXP_value* GTE_data_reg = GTE_data_reg_mem;
static PGXP_value* GTE_ctrl_reg = GTE_ctrl_reg_mem;

void PGXP_InitGTE()
{
  memset(GTE_data_reg_mem, 0, sizeof(GTE_data_reg_mem));
  memset(GTE_ctrl_reg_mem, 0, sizeof(GTE_ctrl_reg_mem));
}

// Instruction register decoding
#define op(_instr) (_instr >> 26)          // The op part of the instruction register
#define func(_instr) ((_instr)&0x3F)       // The funct part of the instruction register
#define sa(_instr) ((_instr >> 6) & 0x1F)  // The sa part of the instruction register
#define rd(_instr) ((_instr >> 11) & 0x1F) // The rd part of the instruction register
#define rt(_instr) ((_instr >> 16) & 0x1F) // The rt part of the instruction register
#define rs(_instr) ((_instr >> 21) & 0x1F) // The rs part of the instruction register
#define imm(_instr) (_instr & 0xFFFF)      // The immediate part of the instruction register

#define SX0 (GTE_data_reg[12].x)
#define SY0 (GTE_data_reg[12].y)
#define SX1 (GTE_data_reg[13].x)
#define SY1 (GTE_data_reg[13].y)
#define SX2 (GTE_data_reg[14].x)
#define SY2 (GTE_data_reg[14].y)

#define SXY0 (GTE_data_reg[12])
#define SXY1 (GTE_data_reg[13])
#define SXY2 (GTE_data_reg[14])
#define SXYP (GTE_data_reg[15])

void PGXP_pushSXYZ2f(float _x, float _y, float _z, unsigned int _v)
{
  static unsigned int uCount = 0;
  low_value temp;
  // push values down FIFO
  SXY0 = SXY1;
  SXY1 = SXY2;

  SXY2.x = _x;
  SXY2.y = _y;
  SXY2.z = (PGXP_GetModes() & PGXP_TEXTURE_CORRECTION) ? _z : 1.f;
  SXY2.value = _v;
  SXY2.flags = VALID_ALL;
  SXY2.count = uCount++;

  // cache value in GPU plugin
  temp.word = _v;
  if (PGXP_GetModes() & PGXP_VERTEX_CACHE)
    PGXP_CacheVertex(temp.x, temp.y, &SXY2);
  else
    PGXP_CacheVertex(0, 0, NULL);

#ifdef GTE_LOG
  GTE_LOG("PGXP_PUSH (%f, %f) %u %u|", SXY2.x, SXY2.y, SXY2.flags, SXY2.count);
#endif
}

void PGXP_pushSXYZ2s(s64 _x, s64 _y, s64 _z, u32 v)
{
  float fx = (float)(_x) / (float)(1 << 16);
  float fy = (float)(_y) / (float)(1 << 16);
  float fz = (float)(_z);

  // if(Config.PGXP_GTE)
  PGXP_pushSXYZ2f(fx, fy, fz, v);
}

#define VX(n) (psxRegs.CP2D.p[n << 1].sw.l)
#define VY(n) (psxRegs.CP2D.p[n << 1].sw.h)
#define VZ(n) (psxRegs.CP2D.p[(n << 1) + 1].sw.l)

int PGXP_NLCIP_valid(u32 sxy0, u32 sxy1, u32 sxy2)
{
  Validate(&SXY0, sxy0);
  Validate(&SXY1, sxy1);
  Validate(&SXY2, sxy2);
  if (((SXY0.flags & SXY1.flags & SXY2.flags & VALID_01) == VALID_01)) // && Config.PGXP_GTE && (Config.PGXP_Mode > 0))
    return 1;
  return 0;
}

float PGXP_NCLIP()
{
  float nclip = ((SX0 * SY1) + (SX1 * SY2) + (SX2 * SY0) - (SX0 * SY2) - (SX1 * SY0) - (SX2 * SY1));

  // ensure fractional values are not incorrectly rounded to 0
  float nclipAbs = fabs(nclip);
  if ((0.1f < nclipAbs) && (nclipAbs < 1.f))
    nclip += (nclip < 0.f ? -1 : 1);

  // float AX = SX1 - SX0;
  // float AY = SY1 - SY0;

  // float BX = SX2 - SX0;
  // float BY = SY2 - SY0;

  //// normalise A and B
  // float mA = sqrt((AX*AX) + (AY*AY));
  // float mB = sqrt((BX*BX) + (BY*BY));

  //// calculate AxB to get Z component of C
  // float CZ = ((AX * BY) - (AY * BX)) * (1 << 12);

  return nclip;
}

static PGXP_value PGXP_MFC2_int(u32 reg)
{
  switch (reg)
  {
    case 15:
      GTE_data_reg[reg] = SXYP = SXY2;
      break;
  }

  return GTE_data_reg[reg];
}

static void PGXP_MTC2_int(PGXP_value value, u32 reg)
{
  switch (reg)
  {
    case 15:
      // push FIFO
      SXY0 = SXY1;
      SXY1 = SXY2;
      SXY2 = value;
      SXYP = SXY2;
      break;

    case 31:
      return;
  }

  GTE_data_reg[reg] = value;
}

////////////////////////////////////
// Data transfer tracking
////////////////////////////////////

void MFC2(int reg)
{
  psx_value val;
  val.d = GTE_data_reg[reg].value;
  switch (reg)
  {
    case 1:
    case 3:
    case 5:
    case 8:
    case 9:
    case 10:
    case 11:
      GTE_data_reg[reg].value = (s32)val.sw.l;
      GTE_data_reg[reg].y = 0.f;
      break;

    case 7:
    case 16:
    case 17:
    case 18:
    case 19:
      GTE_data_reg[reg].value = (u32)val.w.l;
      GTE_data_reg[reg].y = 0.f;
      break;

    case 15:
      GTE_data_reg[reg] = SXY2;
      break;

    case 28:
    case 29:
      //	psxRegs.CP2D.p[reg].d = LIM(IR1 >> 7, 0x1f, 0, 0) | (LIM(IR2 >> 7, 0x1f, 0, 0) << 5) | (LIM(IR3 >> 7, 0x1f, 0,
      // 0) << 10);
      break;
  }
}

void PGXP_GTE_MFC2(u32 instr, u32 rtVal, u32 rdVal)
{
  // CPU[Rt] = GTE_D[Rd]
  Validate(&GTE_data_reg[rd(instr)], rdVal);
  // MFC2(rd(instr));
  CPU_reg[rt(instr)] = GTE_data_reg[rd(instr)];
  CPU_reg[rt(instr)].value = rtVal;
}

void PGXP_GTE_MTC2(u32 instr, u32 rdVal, u32 rtVal)
{
  // GTE_D[Rd] = CPU[Rt]
  Validate(&CPU_reg[rt(instr)], rtVal);
  PGXP_MTC2_int(CPU_reg[rt(instr)], rd(instr));
  GTE_data_reg[rd(instr)].value = rdVal;
}

void PGXP_GTE_CFC2(u32 instr, u32 rtVal, u32 rdVal)
{
  // CPU[Rt] = GTE_C[Rd]
  Validate(&GTE_ctrl_reg[rd(instr)], rdVal);
  CPU_reg[rt(instr)] = GTE_ctrl_reg[rd(instr)];
  CPU_reg[rt(instr)].value = rtVal;
}

void PGXP_GTE_CTC2(u32 instr, u32 rdVal, u32 rtVal)
{
  // GTE_C[Rd] = CPU[Rt]
  Validate(&CPU_reg[rt(instr)], rtVal);
  GTE_ctrl_reg[rd(instr)] = CPU_reg[rt(instr)];
  GTE_ctrl_reg[rd(instr)].value = rdVal;
}

////////////////////////////////////
// Memory Access
////////////////////////////////////
void PGXP_GTE_LWC2(u32 instr, u32 rtVal, u32 addr)
{
  // GTE_D[Rt] = Mem[addr]
  PGXP_value val;
  ValidateAndCopyMem(&val, addr, rtVal);
  PGXP_MTC2_int(val, rt(instr));
}

void PGXP_GTE_SWC2(u32 instr, u32 rtVal, u32 addr)
{
  //  Mem[addr] = GTE_D[Rt]
  Validate(&GTE_data_reg[rt(instr)], rtVal);
  WriteMem(&GTE_data_reg[rt(instr)], addr);
}

// pgxp_gpu.c
/////////////////////////////////
//// Blade_Arma's Vertex Cache (CatBlade?)
/////////////////////////////////
const unsigned int mode_init = 0;
const unsigned int mode_write = 1;
const unsigned int mode_read = 2;
const unsigned int mode_fail = 3;

PGXP_value vertexCache[0x800 * 2][0x800 * 2];

unsigned int baseID = 0;
unsigned int lastID = 0;
unsigned int cacheMode = 0;

unsigned int IsSessionID(unsigned int vertID)
{
  // No wrapping
  if (lastID >= baseID)
    return (vertID >= baseID);

  // If vertID is >= baseID it is pre-wrap and in session
  if (vertID >= baseID)
    return 1;

  // vertID is < baseID, If it is <= lastID it is post-wrap and in session
  if (vertID <= lastID)
    return 1;

  return 0;
}

void PGXP_CacheVertex(short sx, short sy, const PGXP_value* _pVertex)
{
  const PGXP_value* pNewVertex = (const PGXP_value*)_pVertex;
  PGXP_value* pOldVertex = NULL;

  if (!pNewVertex)
  {
    cacheMode = mode_fail;
    return;
  }

  // if (bGteAccuracy)
  {
    if (cacheMode != mode_write)
    {
      // Initialise cache on first use
      if (cacheMode == mode_init)
        memset(vertexCache, 0x00, sizeof(vertexCache));

      // First vertex of write session (frame?)
      cacheMode = mode_write;
      baseID = pNewVertex->count;
    }

    lastID = pNewVertex->count;

    if (sx >= -0x800 && sx <= 0x7ff && sy >= -0x800 && sy <= 0x7ff)
    {
      pOldVertex = &vertexCache[sy + 0x800][sx + 0x800];

      // To avoid ambiguity there can only be one valid entry per-session
      if (0) //(IsSessionID(pOldVertex->count) && (pOldVertex->value == pNewVertex->value))
      {
        // check to ensure this isn't identical
        if ((fabsf(pOldVertex->x - pNewVertex->x) > 0.1f) || (fabsf(pOldVertex->y - pNewVertex->y) > 0.1f) ||
            (fabsf(pOldVertex->z - pNewVertex->z) > 0.1f))
        {
          *pOldVertex = *pNewVertex;
          pOldVertex->gFlags = 5;
          return;
        }
      }

      // Write vertex into cache
      *pOldVertex = *pNewVertex;
      pOldVertex->gFlags = 1;
    }
  }
}

PGXP_value* PGXP_GetCachedVertex(short sx, short sy)
{
  // if (bGteAccuracy)
  {
    if (cacheMode != mode_read)
    {
      if (cacheMode == mode_fail)
        return NULL;

      // Initialise cache on first use
      if (cacheMode == mode_init)
        memset(vertexCache, 0x00, sizeof(vertexCache));

      // First vertex of read session (frame?)
      cacheMode = mode_read;
    }

    if (sx >= -0x800 && sx <= 0x7ff && sy >= -0x800 && sy <= 0x7ff)
    {
      // Return pointer to cache entry
      return &vertexCache[sy + 0x800][sx + 0x800];
    }
  }

  return NULL;
}

bool PGXP_GetVertex(u32 addr, u32 value, int x, int y, int xOffs, int yOffs, float* out_x, float* out_y, float* out_w)
{
  PGXP_value* vert = ReadMem(addr);

  float px, py, pw;
  bool valid_w;

  if (vert && ((vert->flags & VALID_01) == VALID_01) && (vert->value == value))
  {
    // There is a value here with valid X and Y coordinates
    px = (vert->x + xOffs);
    py = (vert->y + yOffs);
    pw = vert->z / 32768.0f;
    valid_w = true;

    if ((vert->flags & VALID_2) != VALID_2)
    {
      // This value does not have a valid W coordinate
      valid_w = false;
    }
  }
  else
  {
    const short psx_x = (short)(value & 0xFFFFu);
    const short psx_y = (short)(value >> 16);

    // Look in cache for valid vertex
    vert = PGXP_GetCachedVertex(psx_x, psx_y);
    if ((vert) && /*(IsSessionID(vert->count)) &&*/ (vert->gFlags == 1))
    {
      // a value is found, it is from the current session and is unambiguous (there was only one value recorded at that
      // position)
      px = vert->x + xOffs;
      py = vert->y + yOffs;
      pw = vert->z / 32768.0f;
      valid_w = 0; // iCB: Getting the wrong w component causes too great an error when using perspective correction so
                   // disable it
    }
    else
    {
      // no valid value can be found anywhere, use the native PSX data
      *out_x = x;
      *out_y = y;
      *out_w = 1.0f;
      return false;
    }
  }

  // clear upper 5 bits in x and y
  px *= (1 << 16);
  py *= (1 << 16);
  x = (float)(((s64)x << 5) >> 5);
  y = (float)(((s64)y << 5) >> 5);
  px /= (1 << 16);
  py /= (1 << 16);

  if (abs((float)x - px) > 2.0 || abs((float)y - py) > 2.0)
  {
    printf("%d %d -> %f %f\n", x, y, px, py);
  }

  *out_x = px;
  *out_y = py;
  *out_w = pw;
  return valid_w;
}

// pgxp_cpu.c

// Instruction register decoding
#define op(_instr) (_instr >> 26)          // The op part of the instruction register
#define func(_instr) ((_instr)&0x3F)       // The funct part of the instruction register
#define sa(_instr) ((_instr >> 6) & 0x1F)  // The sa part of the instruction register
#define rd(_instr) ((_instr >> 11) & 0x1F) // The rd part of the instruction register
#define rt(_instr) ((_instr >> 16) & 0x1F) // The rt part of the instruction register
#define rs(_instr) ((_instr >> 21) & 0x1F) // The rs part of the instruction register
#define imm(_instr) (_instr & 0xFFFF)      // The immediate part of the instruction register

void PGXP_InitCPU()
{
  memset(CPU_reg_mem, 0, sizeof(CPU_reg_mem));
  memset(CP0_reg_mem, 0, sizeof(CP0_reg_mem));
}

// invalidate register (invalid 8 bit read)
void InvalidLoad(u32 addr, u32 code, u32 value)
{
  u32 reg = ((code >> 16) & 0x1F); // The rt part of the instruction register
  PGXP_value* pD = NULL;
  PGXP_value p;

  p.x = p.y = -1337; // default values

  // p.valid = 0;
  // p.count = value;
  pD = ReadMem(addr);

  if (pD)
  {
    p.count = addr;
    p = *pD;
  }
  else
  {
    p.count = value;
  }

  p.flags = 0;

  // invalidate register
  CPU_reg[reg] = p;
}

// invalidate memory address (invalid 8 bit write)
void InvalidStore(u32 addr, u32 code, u32 value)
{
  u32 reg = ((code >> 16) & 0x1F); // The rt part of the instruction register
  PGXP_value* pD = NULL;
  PGXP_value p;

  pD = ReadMem(addr);

  p.x = p.y = -2337;

  if (pD)
    p = *pD;

  p.flags = 0;
  p.count = (reg * 1000) + value;

  // invalidate memory
  WriteMem(&p, addr);
}

void PGXP_CPU_LW(u32 instr, u32 rtVal, u32 addr)
{
  // Rt = Mem[Rs + Im]
  ValidateAndCopyMem(&CPU_reg[rt(instr)], addr, rtVal);
}

void PGXP_CPU_LB(u32 instr, u8 rtVal, u32 addr)
{
  InvalidLoad(addr, instr, 116);
}

void PGXP_CPU_LBU(u32 instr, u8 rtVal, u32 addr)
{
  InvalidLoad(addr, instr, 116);
}

void PGXP_CPU_LH(u32 instr, u16 rtVal, u32 addr)
{
  // Rt = Mem[Rs + Im] (sign extended)
  psx_value val;
  val.sd = (s32)(s16)rtVal;
  ValidateAndCopyMem16(&CPU_reg[rt(instr)], addr, val.d, 1);
}

void PGXP_CPU_LHU(u32 instr, u16 rtVal, u32 addr)
{
  // Rt = Mem[Rs + Im] (zero extended)
  psx_value val;
  val.d = rtVal;
  val.w.h = 0;
  ValidateAndCopyMem16(&CPU_reg[rt(instr)], addr, val.d, 0);
}

void PGXP_CPU_SB(u32 instr, u8 rtVal, u32 addr)
{
  InvalidStore(addr, instr, 208);
}

void PGXP_CPU_SH(u32 instr, u16 rtVal, u32 addr)
{
  // validate and copy half value
  MaskValidate(&CPU_reg[rt(instr)], rtVal, 0xFFFF, VALID_0);
  WriteMem16(&CPU_reg[rt(instr)], addr);
}

void PGXP_CPU_SW(u32 instr, u32 rtVal, u32 addr)
{
  // Mem[Rs + Im] = Rt
  Validate(&CPU_reg[rt(instr)], rtVal);
  WriteMem(&CPU_reg[rt(instr)], addr);
}

} // namespace PGXP