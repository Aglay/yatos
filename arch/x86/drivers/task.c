/*************************************************
 *   Author: Ray Huang
 *   Date  : 2017/4/19
 *   Email : rayhuang@126.com
 *   Desc  : task low lelve
 ************************************************/
#include <arch/system.h>
#include <arch/task.h>
#include <arch/regs.h>
static struct tss task_tss = {
  .ss0 = GDT_KERNEL_DS,
};

void task_arch_init()
{
  unsigned long tss_addr = (unsigned long)&task_tss;
  unsigned long len = 0x67;
  uint32 * tss_des = (uint32 *)GDT_TSS_BASE;
  tss_des[0] = (len & 0xffff) | ((tss_addr & 0xffff) << 16);
  tss_des[1] = ((tss_addr >> 16) & 0xff) | (0x89 << 8);
  tss_des[1] |= ((len >> 16) & 0xf) << 16;
  tss_des[1] |= ((tss_addr >> 24) & 0xff) << 24;

  asm("movl $0x30, %eax\n"
      "ltr %ax\n"
      );
}

void task_arch_befor_launch(struct task* task)
{
  task_tss.esp0 = task->kernel_stack;
  task_tss.cr3 = vaddr_to_paddr(task->mm_info->mm_table_vaddr);
}

extern void irq_common_ret();
void task_arch_init_run_context(struct task * task, unsigned long ret_val)
{
  // not ss esp ,so we should sub 8
  struct pt_regs * regs = (struct pt_regs *)(task->kernel_stack - sizeof(*regs));
  regs->eax = ret_val;

  //now we need create a frame for first schedule
  struct task_sche_frame * frame = (struct task_sche_frame *)(task->kernel_stack - sizeof(*regs) - sizeof(*frame));
  frame->eip = irq_common_ret;//directly goto irq_common_ret;
  task->cur_stack = (unsigned long)frame;
}
