/****************************************************************************
 * arch/arm64/src/common/arm64_cache.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/cache.h>
#include <nuttx/irq.h>

#include <nuttx/arch.h>
#include <arch/barriers.h>
#include <arch/irq.h>
#include <arch/chip/chip.h>
#include <nuttx/spinlock.h>
#include <assert.h>

#include "arm64_arch.h"
#include "arm64_internal.h"
#include "arm64_mmu.h"

/****************************************************************************
 * Pre-processor Macros
 ****************************************************************************/

/* Common operations for the caches
 *
 * WB means write-back and intends to transfer dirty cache lines to memory in
 * a copy-back cache policy. May be a no-op in write-back cache policy.
 *
 * INVD means invalidate and will mark cache lines as not valid. A future
 * access to the associated address is guaranteed to generate a memory fetch.
 *
 * armv8 data cache instruction:
 *
 * DC CIVAC (WB+INVD):
 *   Data or unified Cache line Clean and Invalidate by VA to PoC
 *   Clean and Invalidate data cache by address to Point of Coherency.
 *
 * DC CVAC (WB):
 *   Data or unified Cache line Clean by VA to PoC
 *   Clean data cache by address to Point of Coherency.
 *
 * DC IVAC (INVD):
 *   Data or unified Cache line Invalidate by VA to PoC
 *   Invalidate data cache by address to Point of Coherency
 */

#define CACHE_OP_WB         BIT(0)
#define CACHE_OP_INVD       BIT(1)
#define CACHE_OP_WB_INVD    (CACHE_OP_WB | CACHE_OP_INVD)

#define LINE_MASK(line)             ((line) - 1)
#define LINE_ALIGN_DOWN(a, line)    ((a) & ~LINE_MASK(line))
#define LINE_ALIGN_UP(a, line) \
  (((a) + LINE_MASK(line)) & ~LINE_MASK(line))

#define dc_ops(op, val)                                          \
  ({                                                             \
    __asm__ volatile ("dc " op ", %x0" : : "r" (val) : "memory"); \
  })

#define ic_ops(op, val)                                          \
  ({                                                             \
    __asm__ volatile ("ic " op ", %0" : : "r" (val) : "memory"); \
  })

/* IC IALLU, Instruction Cache Invalidate All to PoU
 * Purpose
 * Invalidate all instruction caches of the PE executing
 * the instruction to the Point of Unification.
 */

static inline void __ic_ialluis(void)
{
  __asm__ volatile ("ic  ialluis" : : : "memory");
}

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static inline uint32_t arm64_cache_get_info(uint32_t *sets, uint32_t *ways,
                                            bool icache)
{
  uint32_t csselr;
  uint32_t ccsidr;

  csselr = read_sysreg(csselr_el1);
  write_sysreg((csselr & ~CSSELR_EL1_IND_MASK) |
               (icache << CSSELR_EL1_IND_SHIFT),
               csselr_el1);

  ccsidr = read_sysreg(ccsidr_el1);

  if (sets)
    {
      *sets = ((ccsidr >> CCSIDR_EL1_SETS_SHIFT) & CCSIDR_EL1_SETS_MASK) + 1;
    }

  if (ways)
    {
      *ways = ((ccsidr >> CCSIDR_EL1_WAYS_SHIFT) & CCSIDR_EL1_WAYS_MASK) + 1;
    }

  write_sysreg(csselr, csselr_el1);

  return (1 << ((ccsidr & CCSIDR_EL1_LN_SZ_MASK) + 2)) * 4;
}

static inline size_t arm64_get_cache_size(bool icache)
{
  uint32_t sets;
  uint32_t ways;
  uint32_t line;

  line = arm64_cache_get_info(&sets, &ways, icache);

  return sets * ways * line;
}

/* operation for data cache by virtual address to PoC */

static inline int arm64_dcache_range(uintptr_t start_addr,
                                     uintptr_t end_addr, int op)
{
  size_t line_size = up_get_dcache_linesize();

  /* Align address to line size */

  if ((start_addr & (line_size - 1)) != 0)
    {
      start_addr = LINE_ALIGN_DOWN(start_addr, line_size);

      if (op == CACHE_OP_INVD)
        {
          dc_ops("civac", start_addr);
          start_addr += line_size;
        }
    }

  while (start_addr < end_addr)
    {
      switch (op)
        {
        case CACHE_OP_WB:
          {
            dc_ops("cvac", start_addr);
            break;
          }

        case CACHE_OP_INVD:
          {
            if (start_addr + line_size <= end_addr)
              {
                dc_ops("ivac", start_addr);
              }
            else
              {
                dc_ops("civac", start_addr);
              }
            break;
          }

        case CACHE_OP_WB_INVD:
          {
            dc_ops("civac", start_addr);
            break;
          }

        default:
          {
            DEBUGASSERT(0);
          }
        }
      start_addr += line_size;
    }

  UP_MB();

  return 0;
}

/* operation for all data cache */

static inline int arm64_dcache_all(int op)
{
  uint64_t  clidr_el1;
  uint64_t  csselr_el1;
  uint64_t  ccsidr_el1;
  uint8_t   loc;
  uint8_t   ctype;
  uint8_t   cache_level;
  uint8_t   line_size;
  uint8_t   way_pos;
  uint32_t  max_ways;
  uint32_t  max_sets;
  uint32_t  dc_val;
  uint32_t  set;
  uint32_t  way;

  /* Data barrier before start */

  UP_DSB();

  clidr_el1 = read_sysreg(clidr_el1);

  loc = (clidr_el1 >> CLIDR_EL1_LOC_SHIFT) & CLIDR_EL1_LOC_MASK;
  if (!loc)
    {
      return 0;
    }

  for (cache_level = 0; cache_level < loc; cache_level++)
    {
      ctype =
        (clidr_el1 >>
         CLIDR_EL1_CTYPE_SHIFT(cache_level)) & CLIDR_EL1_CTYPE_MASK;

      /* No data cache, continue */

      if (ctype < 2)
        {
          continue;
        }

      /* select cache level */

      csselr_el1 = cache_level << 1;
      write_sysreg(csselr_el1, csselr_el1);
      UP_ISB();

      ccsidr_el1    = read_sysreg(ccsidr_el1);
      line_size     =
        (ccsidr_el1 >> CCSIDR_EL1_LN_SZ_SHIFT & CCSIDR_EL1_LN_SZ_MASK) + 4;
      max_ways =
        (ccsidr_el1 >> CCSIDR_EL1_WAYS_SHIFT) & CCSIDR_EL1_WAYS_MASK;
      max_sets =
        (ccsidr_el1 >> CCSIDR_EL1_SETS_SHIFT) & CCSIDR_EL1_SETS_MASK;

      /* 32-log2(ways), bit position of way in DC operand */

      way_pos = __builtin_clz(max_ways);

      for (set = 0; set <= max_sets; set++)
        {
          for (way = 0; way <= max_ways; way++)
            {
              /* way number, aligned to pos in DC operand */

              dc_val = way << way_pos;

              /* cache level, aligned to pos in DC operand */

              dc_val |= csselr_el1;

              /* set number, aligned to pos in DC operand */

              dc_val |= set << line_size;
              switch (op)
                {
                  case CACHE_OP_WB:
                    {
                      dc_ops("csw", dc_val);
                      break;
                    }

                  case CACHE_OP_INVD:
                    {
                      dc_ops("isw", dc_val);
                      break;
                    }

                  case CACHE_OP_WB_INVD:
                    {
                      dc_ops("cisw", dc_val);
                      break;
                    }
                  default:
                    {
                      DEBUGASSERT(0);
                    }
                }
            }
        }
    }

  /* Restore csselr_el1 to level 0 */

  write_sysreg(0, csselr_el1);
  UP_MB();

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef CONFIG_ARCH_ICACHE

/****************************************************************************
 * Name: up_get_icache_linesize
 *
 * Description:
 *   Get icache linesize
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Cache line size
 *
 ****************************************************************************/

size_t up_get_icache_linesize(void)
{
  static uint32_t clsize;

  if (clsize == 0)
    {
      clsize = arm64_cache_get_info(NULL, NULL, true);
    }

  return clsize;
}

/****************************************************************************
 * Name: up_get_icache_size
 *
 * Description:
 *   Get icache size
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Cache size
 *
 ****************************************************************************/

size_t up_get_icache_size(void)
{
  static uint32_t csize;

  if (csize == 0)
    {
      csize = arm64_get_cache_size(true);
    }

  return csize;
}

/****************************************************************************
 * Name: up_invalidate_icache_all
 *
 * Description:
 *   Invalidate all instruction caches to PoU, also flushes branch target
 *   cache
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void up_invalidate_icache_all(void)
{
  __ic_ialluis();
}

/****************************************************************************
 * Name: up_invalidate_icache
 *
 * Description:
 *   Validate the specified range instruction cache as PoU,
 *   and flush the branch target cache
 *
 * Input Parameters:
 *   start - virtual start address of region
 *   end   - virtual end address of region + 1
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void up_invalidate_icache(uintptr_t start, uintptr_t end)
{
  size_t line_size = up_get_icache_linesize();

  /* Align address to line size */

  start = LINE_ALIGN_DOWN(start, line_size);

  UP_DSB();

  while (start < end)
    {
      ic_ops("ivau", start);
      start += line_size;
    }

  UP_ISB();
}

/****************************************************************************
 * Name: up_enable_icache
 *
 * Description:
 *   Enable the I-Cache
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void up_enable_icache(void)
{
  uint64_t value = read_sysreg(sctlr_el1);
  write_sysreg((value | SCTLR_I_BIT), sctlr_el1);
  UP_ISB();
}

/****************************************************************************
 * Name: up_disable_icache
 *
 * Description:
 *   Disable the I-Cache
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void up_disable_icache(void)
{
  uint64_t value = read_sysreg(sctlr_el1);
  write_sysreg((value & ~SCTLR_I_BIT), sctlr_el1);
  UP_ISB();
}

#endif /* CONFIG_ARCH_ICACHE */

#ifdef CONFIG_ARCH_DCACHE

/****************************************************************************
 * Name: up_invalidate_dcache
 *
 * Description:
 *   Invalidate the data cache within the specified region; we will be
 *   performing a DMA operation in this region and we want to purge old data
 *   in the cache.
 *
 * Input Parameters:
 *   start - virtual start address of region
 *   end   - virtual end address of region + 1
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   This operation is not atomic.  This function assumes that the caller
 *   has exclusive access to the address range so that no harm is done if
 *   the operation is preempted.
 *
 ****************************************************************************/

void up_invalidate_dcache(uintptr_t start, uintptr_t end)
{
  arm64_dcache_range(start, end, CACHE_OP_INVD);
}

/****************************************************************************
 * Name: up_invalidate_dcache_all
 *
 * Description:
 *   Invalidate the entire contents of D cache.
 *
 *   NOTE: This function forces L1 and L2 cache operations to be atomic
 *   by disabling interrupts.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void up_invalidate_dcache_all(void)
{
  arm64_dcache_all(CACHE_OP_INVD);
}

/****************************************************************************
 * Name: up_get_dcache_linesize
 *
 * Description:
 *   Get dcache linesize
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Cache line size
 *
 ****************************************************************************/

size_t up_get_dcache_linesize(void)
{
  static uint32_t clsize;

  if (clsize == 0)
    {
      clsize = arm64_cache_get_info(NULL, NULL, false);
    }

  return clsize;
}

/****************************************************************************
 * Name: up_get_dcache_size
 *
 * Description:
 *   Get dcache size
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Cache size
 *
 ****************************************************************************/

size_t up_get_dcache_size(void)
{
  static uint32_t csize;

  if (csize == 0)
    {
      csize = arm64_get_cache_size(false);
    }

  return csize;
}

/****************************************************************************
 * Name: up_clean_dcache
 *
 * Description:
 *   Clean the data cache within the specified region by flushing the
 *   contents of the data cache to memory.
 *
 * Input Parameters:
 *   start - virtual start address of region
 *   end   - virtual end address of region + 1
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   This operation is not atomic.  This function assumes that the caller
 *   has exclusive access to the address range so that no harm is done if
 *   the operation is preempted.
 *
 ****************************************************************************/

void up_clean_dcache(uintptr_t start, uintptr_t end)
{
  arm64_dcache_range(start, end, CACHE_OP_WB);
}

/****************************************************************************
 * Name: up_clean_dcache_all
 *
 * Description:
 *   Clean the entire data cache within the specified region by flushing the
 *   contents of the data cache to memory.
 *
 *   NOTE: This operation is un-necessary if the DCACHE is configured in
 *   write-through mode.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   This operation is not atomic.  This function assumes that the caller
 *   has exclusive access to the address range so that no harm is done if
 *   the operation is preempted.
 *
 ****************************************************************************/

void up_clean_dcache_all(void)
{
  arm64_dcache_all(CACHE_OP_WB);
}

/****************************************************************************
 * Name: up_enable_dcache
 *
 * Description:
 *   Enable the D-Cache
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void up_enable_dcache(void)
{
  uint64_t value = read_sysreg(sctlr_el1);

  /* Check if the D-Cache is enabled */

  if ((value & SCTLR_C_BIT) != 0)
    {
      return;
    }

  up_invalidate_dcache_all();

  value = read_sysreg(sctlr_el1);
  write_sysreg((value | SCTLR_C_BIT), sctlr_el1);
  UP_ISB();
}

/****************************************************************************
 * Name: up_disable_dcache
 *
 * Description:
 *   Disable the D-Cache
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void up_disable_dcache(void)
{
  uint64_t value = read_sysreg(sctlr_el1);
  write_sysreg((value & ~SCTLR_C_BIT), sctlr_el1);
  UP_ISB();
}

/****************************************************************************
 * Name: up_flush_dcache
 *
 * Description:
 *   Flush the data cache within the specified region by cleaning and
 *   invalidating the D cache.
 *
 * Input Parameters:
 *   start - virtual start address of region
 *   end   - virtual end address of region + 1
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   This operation is not atomic.  This function assumes that the caller
 *   has exclusive access to the address range so that no harm is done if
 *   the operation is preempted.
 *
 ****************************************************************************/

void up_flush_dcache(uintptr_t start, uintptr_t end)
{
  arm64_dcache_range(start, end, CACHE_OP_WB_INVD);
}

/****************************************************************************
 * Name: up_flush_dcache_all
 *
 * Description:
 *   Flush the entire data cache by cleaning and invalidating the D cache.
 *
 *   NOTE: If DCACHE write-through is configured, then this operation is the
 *   same as up_invalidate_cache_all().
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   This operation is not atomic.  This function assumes that the caller
 *   has exclusive access to the address range so that no harm is done if
 *   the operation is preempted.
 *
 ****************************************************************************/

void up_flush_dcache_all(void)
{
  arm64_dcache_all(CACHE_OP_WB_INVD);
}

/****************************************************************************
 * Name: up_coherent_dcache
 *
 * Description:
 *   Ensure that the I and D caches are coherent within specified region
 *   by cleaning the D cache (i.e., flushing the D cache contents to memory
 *   and invalidating the I cache. This is typically used when code has been
 *   written to a memory region, and will be executed.
 *
 * Input Parameters:
 *   addr - virtual start address of region
 *   len  - Size of the address region in bytes
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void up_coherent_dcache(uintptr_t addr, size_t len)
{
  if (len > 0)
    {
      arm64_dcache_range(addr, addr + len, CACHE_OP_WB_INVD);
      up_invalidate_icache_all();
    }
}

#endif /* CONFIG_ARCH_DCACHE */
