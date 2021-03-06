/*
 *  System type define and memory map define
 *
 *  Copyright (C) 2017 ese@ccnt.zju
 *
 *  ---------------------------------------------------
 *  Started at 2017/3/30 by Ray
 *
 *  ---------------------------------------------------
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.
 */

#ifndef __ARCH_SYSTEM_H
#define __ARCH_SYSTEM_H

typedef int int32;
typedef short int16;
typedef char int8;
typedef unsigned int uint32;
typedef unsigned short uint16;
typedef unsigned char uint8;
typedef unsigned char bool;
typedef unsigned int size_t;
#define  NULL (void *)0

//========= MM MAP ================================/
#define PHY_MM_START 0x100000
#define PHY_MM_SIZE  (1024 * 1024 *124)

#define PAGE_SIZE  (4 * 1024)
#define PAGE_SHIFT 12

#define PAGE_ALIGN(addr)  ((unsigned long)(addr) & ~0xfff)
#define PAGE_OFFSET(addr) ((unsigned long)(addr) & 0xfff)

#define KERNEL_VMM_START 0xc0000000
#define KERNEL_SIZE 0x400000
#define KERNEL_END KERNEL_VMM_START + KERNEL_SIZE
//--- 0xc0000000 + 4MB
#define VGA_PHY_START 0x8b000
#define VGA_VMM_START KERNEL_END
#define VGA_SIZE      PAGE_SIZE

//----0xc0000000 + 4MB + 1KB
#define INIT_PDT_TABLE_START (VGA_VMM_START + VGA_SIZE)

//----0xc0000000 + 4MB + 1KB + 1KB
#define INIT_PET_TABLES_START (INIT_PDT_TABLE_START + PAGE_SIZE)
#define INIT_PET_TABLES_NUM (PHY_MM_SIZE / (4*1024*1024))

//----0xc0000000 + 4MB + 1KB + 1KB + INIT_PET_TABLES_NUM * PAGE_SIZE
#define __FREE_VMM_START (INIT_PET_TABLES_START + INIT_PET_TABLES_NUM * PAGE_SIZE)
#define FREE_VMM_START ((__FREE_VMM_START + (PAGE_SIZE - 1)) / PAGE_SIZE * PAGE_SIZE) // ALIGN PAGE_SIZE

#define FREE_PMM_START (FREE_VMM_START - KERNEL_VMM_START + PHY_MM_START)

//total num of free page
#define FREE_PAGE_TOTAL ((PHY_MM_START +  PHY_MM_SIZE - FREE_PMM_START) / PAGE_SIZE)

//===================   GDT   ========================
#define GDT_BASE (KERNEL_END - PAGE_SIZE)
#define GDT_KERNEL_CS 0x10
#define GDT_KERNEL_DS 0X18
#define GDT_USER_CS 0x23
#define GDT_USER_DS 0x2B
#define GDT_TSS 0x30
#define GDT_TSS_BASE (GDT_BASE + 48)

#define IDT_BASE (GDT_BASE - PAGE_SIZE)

#endif /* __ARCH_SYSTEM_H */
