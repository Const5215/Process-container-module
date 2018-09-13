//////////////////////////////////////////////////////////////////////
//                      North Carolina State University
//
//
//
//                             Copyright 2016
//
////////////////////////////////////////////////////////////////////////
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the GNU General Public License,
// version 2, as published by the Free Software Foundation.
//
// This program is distributed in the hope it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
//
////////////////////////////////////////////////////////////////////////
//
//   Author:  Hung-Wei Tseng, Yu-Chia Liu
//
//   Description:
//     Core of Kernel Module for Processor Container
//
////////////////////////////////////////////////////////////////////////

#include "processor_container.h"

#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/kthread.h>

#include <linux/list.h>

//my define of two structures
struct task_list_node {
    struct list_head list;
    struct task_struct* task_id;
};

struct container_list_node {
    struct list_head list;
    int cid;
    struct list_head* task_head;//should be the head of a linklist of task_list_node
    struct list_head* running_task;//should be the executing front
};

extern struct list_head *container_list_head;
extern struct list_head *working_container;
extern struct mutex *container_lock;
/**
 * Delete the task in the container.
 * 
 * external functions needed:
 * mutex_lock(), mutex_unlock(), wake_up_process(), 
 */
int processor_container_delete(struct processor_container_cmd __user *user_cmd)
{
    //defunc the running one in working container, then deal with others.
    struct container_list_node *target_container;
    struct task_list_node *target_task;
    mutex_lock(container_lock);
    //printk("Delete Triggered. id:%d\n", current->pid);
    target_container = list_entry(working_container, struct container_list_node, list);
    target_task = list_entry(target_container->running_task, struct task_list_node, list);
    target_container->running_task = target_container->running_task->next;
    list_del(target_container->running_task->prev);//kfree(&target_task);
    if (target_container->running_task == target_container->task_head)
        target_container->running_task = target_container->running_task->next;
    //no task left, destroy the container
    if (target_container->running_task == target_container->task_head) {
        working_container = working_container->next;
        list_del(working_container->prev);//kfree(&target_container);
    }
    //activate the next task
    if (working_container == container_list_head)
        working_container = working_container->next;
    if (working_container != container_list_head) {
        target_container = list_entry(working_container, struct container_list_node, list);
        target_task = list_entry(target_container->running_task, struct task_list_node, list);
        //printk("Trying to wake:%d\n",target_task->task_id->pid);
        wake_up_process(target_task->task_id);
    }
    else printk("No waking.\n");
    //printk("Delete done.");
    mutex_unlock(container_lock);
    return 0;
}

/**
 * Create a task in the corresponding container.
 * external functions needed:
 * copy_from_user(), mutex_lock(), mutex_unlock(), set_current_state(), schedule()
 * 
 * external variables needed:
 * struct task_struct* current  
 */
int processor_container_create(struct processor_container_cmd __user *user_cmd)
{
    //find exist containers first, compare cid
    struct list_head *ptr;
    struct container_list_node *entry;
    struct task_list_node* new_task;
    mutex_lock(container_lock);
    //printk("Create triggered.\n");
    for (ptr = container_list_head->next; ptr != container_list_head; ptr = ptr->next) {
        entry = list_entry(ptr, struct container_list_node, list);
        if (entry->cid == user_cmd->cid) {
            //insert into found container
            new_task = (struct task_list_node *) kcalloc(1, sizeof(struct task_list_node), GFP_KERNEL);
            //new_task->task_id = (struct task_struct *) kcalloc(1, sizeof(struct task_struct), GFP_KERNEL);
            new_task->task_id = current;
            //copy_from_user(new_task->task_id, current, sizeof(struct task_struct));
            list_add_tail(&new_task->list, entry->task_head);
            //sleep current process
            //printk("task created. id:%d\n", new_task->task_id->pid);
            set_current_state(TASK_INTERRUPTIBLE);
            mutex_unlock(container_lock);
            schedule();
            return 0;
        }
    }
    //create a new container and a new task
    entry = (struct container_list_node *) kcalloc(1, sizeof(struct container_list_node), GFP_KERNEL);
    entry->cid = user_cmd->cid;
    entry->task_head = (struct list_head *) kcalloc(1, sizeof(struct list_head), GFP_KERNEL);
    INIT_LIST_HEAD(entry->task_head);
    new_task = (struct task_list_node *) kcalloc(1, sizeof(struct task_list_node), GFP_KERNEL);
    //new_task->task_id = (struct task_struct *) kcalloc(1, sizeof(struct task_struct), GFP_KERNEL);
    new_task->task_id = current;
    //copy_from_user(new_task->task_id, current, sizeof(struct task_struct));
    list_add_tail(&new_task->list, entry->task_head);
    entry->running_task = &new_task->list;
    list_add_tail(&entry->list, container_list_head);
    if (working_container != container_list_head) {
        mutex_unlock(container_lock);
        set_current_state(TASK_INTERRUPTIBLE);
        schedule();
    }
    else {
        working_container = &entry->list;
    }
    //printk("Container & task created. id:%d\n", new_task->task_id->pid);
    mutex_unlock(container_lock);
    return 0;
}

/**
 * switch to the next task in the next container
 * 
 * external functions needed:
 * mutex_lock(), mutex_unlock(), wake_up_process(), set_current_state(), schedule()
 */
int processor_container_switch(struct processor_container_cmd __user *user_cmd)
{   
    //move the running task of working_container and move working_container itself.
    struct list_head* now_task;
    struct container_list_node *entry, *next_entry;
    struct task_list_node *next_task;
    mutex_lock(container_lock);
    //printk("Switch triggered.\n");
    entry = list_entry(working_container, struct container_list_node, list);
    //find next running task and skip the meaningless head pointer
    now_task = entry->running_task;
    entry->running_task = entry->running_task->next;
    if (entry->running_task == entry->task_head)
        entry->running_task = entry->running_task->next;
    working_container = working_container->next;
    if (working_container == container_list_head)
        working_container = working_container->next;
    next_entry = list_entry(working_container, struct container_list_node, list);
    if (next_entry->running_task != now_task) {
        next_task = list_entry(next_entry->running_task, struct task_list_node, list);
        mutex_unlock(container_lock);
        wake_up_process(next_task->task_id);
        set_current_state(TASK_INTERRUPTIBLE);
        //printk("Switch success. Now:%d\n", next_task->task_id->pid);
        schedule();
    }   
    //printk("Switch done.\n");
    mutex_unlock(container_lock);
    return 0;
}

/**
 * control function that receive the command in user space and pass arguments to
 * corresponding functions.
 */
int processor_container_ioctl(struct file *filp, unsigned int cmd,
                              unsigned long arg)
{
    switch (cmd)
    {
    case PCONTAINER_IOCTL_CSWITCH:
        return processor_container_switch((void __user *)arg);
    case PCONTAINER_IOCTL_CREATE:
        return processor_container_create((void __user *)arg);
    case PCONTAINER_IOCTL_DELETE:
        return processor_container_delete((void __user *)arg);
    default:
        return -ENOTTY;
    }
}
