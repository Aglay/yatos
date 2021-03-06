/*
 *  Task schedule system.
 *  This file manage all task and run schedule system.
 *
 *  Copyright (C) 2017 ese@ccnt.zju
 *
 *  ---------------------------------------------------
 *  Started at 2017/4/20 by Ray
 *
 *  ---------------------------------------------------
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.
 */

#include <yatos/schedule.h>
#include <yatos/task.h>
#include <arch/task.h>
#include <yatos/task_vmm.h>
#include <yatos/irq.h>
#include <arch/irq.h>
#include <yatos/mm.h>
#include <yatos/slab.h>
#include <arch/asm.h>

static struct list_head task_list;
static struct list_head ready_listA;
static struct list_head ready_listB;
static struct list_head * run_list;
static struct list_head * time_up_list;
static struct task * task_current;
static struct irq_action sched_irq_action;

/*
 * Check current run_list, if it is empty, swap run_list and time_up_list;
 */
static void check_run_list_reload()
{
  struct list_head * tmp;
  if (list_empty(run_list)){
    tmp = run_list;
    run_list = time_up_list;
    time_up_list = tmp;
  }
}

/*
 * This is the irq handler of timer irq.
 * This function decrease the time count of current task.
 */
static void do_schedule_count(void *private, struct pt_regs * regs)
{
  if (task_current->need_sched || task_current->state != TASK_STATE_RUN)
    return ;
  task_current->remain_click--;
  if (!task_current->remain_click){
    list_del(&(task_current->run_list_entry));
    list_add(&(task_current->run_list_entry), time_up_list);
    task_current->remain_click = MAX_TASK_RUN_CLICK;
    task_current->need_sched = 1;
  }
}

/*
 * Initate schedule system.
 */
void task_schedule_init()
{
  INIT_LIST_HEAD(&task_list);
  run_list = &ready_listA;
  time_up_list = &ready_listB;
  INIT_LIST_HEAD(run_list);
  INIT_LIST_HEAD(time_up_list);
  irq_action_init(&sched_irq_action);
  sched_irq_action.action = do_schedule_count;
  irq_regist(IRQ_TIMER, &sched_irq_action);
}

/*
 * This function will really switch current task from "prev" to "next"
 */
static void task_switch_to(struct task * prev, struct task *next)
{
  task_arch_befor_launch(next);
  task_vmm_switch_to(prev->mm_info, next->mm_info);
  task_arch_switch_to(prev, next);
}

/*
 * This function select a new task and switch to it.
 * If there is no runable task, system will be halted and waitting for any irq.
 */
void task_schedule()
{
  check_run_list_reload();
  uint32 irq_save;
  while (list_empty(run_list)){
    irq_save = arch_irq_save();
    arch_irq_enable();
    system_hlt();
    arch_irq_recover(irq_save);
  }
  struct task * next = container_of(run_list->next, struct task, run_list_entry);
  struct task * pre = task_current;
  task_current = next;
  task_switch_to(pre, next);
}

/*
 * If task->need_sched was set, task_schedule() will be called.
 * This function will be called when the code stream return to user space.
 */
void task_check_schedule()
{
  if (task_current->need_sched){
    task_current->need_sched = 0;
    task_schedule();
  }
}

/*
 * Add a new task to task hash.
 * The task will also be add to run_list.
 */
void task_add_new_task(struct task * new)
{
  uint32 irq_save = arch_irq_save();
  arch_irq_disable();
  list_add(&(new->task_list_entry), &(task_list));
  list_add(&(new->run_list_entry), run_list);
  if (task_current == NULL)
    task_current = new;

  arch_irq_recover(irq_save);
}

/*
 * Delete task from task hash.
 * This function doesn't remove task from run_list since the task must be a zombie task and not
 * in the run_list at all.
 */
void task_delete_task(struct task* task)
{
  uint32 irq_save = arch_irq_save();
  arch_irq_disable();
  list_del(&(task->task_list_entry));
  arch_irq_recover(irq_save);
}

/*
 * Set task state to be TASK_STATE_ZOMBIE.
 * This function may remove the task from run_list.
 */
void task_tobe_zombie(struct task* task)
{
  uint32 irq_save = arch_irq_save();
  arch_irq_disable();
  if (task->state == TASK_STATE_RUN)
    list_del(&(task->run_list_entry));
  task->state = TASK_STATE_ZOMBIE;
  arch_irq_recover(irq_save);
}

/*
 * Set task state to be TASK_STATE_BLOCK, and remove from run_list if nessary.
 */
void task_block(struct task* task)
{
  uint32 irq_save = arch_irq_save();
  arch_irq_disable();
  if (task->state == TASK_STATE_RUN)
    list_del(&(task->run_list_entry));
  task->state = TASK_STATE_BLOCK;
  arch_irq_recover(irq_save);
}

/*
 * Set task state to be TASK_STATE_RUN and add to run_list.
 */
void task_ready_to_run(struct task* task)
{
  uint32 irq_save = arch_irq_save();
  arch_irq_disable();
  if (task->state != TASK_STATE_RUN){
    task->state = TASK_STATE_RUN;
    if (task->remain_click)
      list_add(&(task->run_list_entry), run_list);
    else{
      list_add(&(task->run_list_entry), time_up_list);
      task->remain_click = MAX_TASK_RUN_CLICK;
    }
    task->need_sched = 1;
  }
  arch_irq_recover(irq_save);
}

/*
 * Get current running task.
 */
struct task * task_get_cur()
{
  return task_current;
}

/*
 * Find a task from task hash by pid.
 */
struct task * task_find_by_pid(int pid)
{
  struct list_head * cur;
  struct task * task;
  list_for_each(cur, &task_list){
    task = container_of(cur, struct task, task_list_entry);
    if (task->pid == pid)
      return task;
  }
  return NULL;
}
