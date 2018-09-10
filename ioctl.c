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
/**
 * Delete the task in the container.
 * 
 * external functions needed:
 * mutex_lock(), mutex_unlock(), wake_up_process(), 
 */
int processor_container_delete(struct processor_container_cmd __user *user_cmd)
{
    //find container entry using cid, defunc the first(running) one, then deal with others.
    struct list_head *ptr;
    struct container_list_node *entry;
    for (ptr = container_list_head->next; ptr != container_list_head; ptr = ptr->next) {
        entry = list_entry(ptr, struct container_list_node, list);
        if (entry->cid == user_cmd->cid) {
            struct list_head *target_pointer = entry->task_head->next;
            struct task_list_node *target_body = list_entry(target_pointer, struct task_list_node, list);
            entry->running_task = target_pointer->next;
            list_del(target_pointer);
            kfree(target_pointer);kfree(target_body);
            //no task left, destroy the container
            if (entry->running_task == entry->task_head) {
                list_del(ptr);
                kfree(ptr);kfree(entry);
            }
            //or activate the next task
            else {
                struct task_list_node *next_task = list_entry(entry->running_task, struct task_list_node, list);
                wake_up_process(next_task->task_id);
            }
            return 0;
        }
    }
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
    for (ptr = container_list_head->next; ptr != container_list_head; ptr = ptr->next) {
        entry = list_entry(ptr, struct container_list_node, list);
        if (entry->cid == user_cmd->cid) {
            //insert into found container
            struct task_list_node* new_task = (struct task_list_node *) kcalloc(1, sizeof(struct task_list_node), GFP_KERNEL);
            copy_from_user(new_task->task_id, current, sizeof(struct task_struct));
            list_add_tail(&new_task->list, entry->task_head);
            //sleep current process
            set_current_state(TASK_INTERRUPTIBLE);
            schedule();
            return 0;
        }
    }
    //create a new container and a new task
    entry = (struct container_list_node *) kcalloc(1, sizeof(struct container_list_node), GFP_KERNEL);
    entry->cid = user_cmd->cid;
    entry->task_head = (struct list_head *) kcalloc(1, sizeof(struct list_head), GFP_KERNEL);
    INIT_LIST_HEAD(entry->task_head);
    struct task_list_node* new_task = (struct task_list_node *) kcalloc(1, sizeof(struct task_list_node), GFP_KERNEL);
    copy_from_user(new_task->task_id, current, sizeof(struct task_struct));
    list_add_tail(&new_task->list, entry->task_head);
    entry->running_task = &new_task->list;
    list_add_tail(&entry->list, container_list_head);
    //should current process set to sleep? Or left running? Not sure but I choose the latter.
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
    //find next container, activate the next one and stop the current one
    struct list_head *ptr;
    struct container_list_node *entry;
    for (ptr = container_list_head->next; ptr != container_list_head; ptr = ptr->next) {
        entry = list_entry(ptr, struct container_list_node, list);
        if (entry->cid == user_cmd->cid) {
            //find next container and skip the meaningless head pointer
            struct list_head *ptr_next = ptr->next;
            if (ptr_next == container_list_head)
                ptr_next = container_list_head->next;
            //find running task in the next container
            struct container_list_node *entry_next = list_entry(ptr_next, struct container_list_node, list);
            struct task_list_node *task_next = list_entry(entry_next->running_task, struct task_list_node, list);
            wake_up_process(task_next->task_id);
            //stop current one
            set_current_state(TASK_INTERRUPTIBLE);
            schedule();
            return 0;
        }
    }    
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
