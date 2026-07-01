#include <drivers/input.h>
#include <logging/printk.h>
#include <mm/heap.h>
#include <multitasking/proc.h>
#include <sync/spinlock.h>

typedef struct {
    uint32_t      pid;
    input_event_t events[INPUT_EVENT_QUEUE_SIZE];
    uint32_t      head;
    uint32_t      tail;
    uint32_t      count;
} input_subscriber_t;

static input_subscriber_t subscribers[MAX_INPUT_SUBSCRIBERS];
static uint32_t num_subscribers = 0;
static spinlock_t input_lock = 0;

void input_init() {
    num_subscribers = 0;
    log_info("Input event system initialized\n");
}

void input_subscribe(uint32_t pid) {
    spinlock_acquire(&input_lock);
    
    for (uint32_t i = 0; i < num_subscribers; i++) {
        if (subscribers[i].pid == pid) {
            spinlock_release(&input_lock);
            return;
        }
    }
    
    if (num_subscribers < MAX_INPUT_SUBSCRIBERS) {
        subscribers[num_subscribers].pid = pid;
        subscribers[num_subscribers].head = 0;
        subscribers[num_subscribers].tail = 0;
        subscribers[num_subscribers].count = 0;
        num_subscribers++;
        log_debug("Input subscriber added: pid=%u\n", pid);
    }
    
    spinlock_release(&input_lock);
}

void input_unsubscribe(uint32_t pid) {
    spinlock_acquire(&input_lock);
    
    for (uint32_t i = 0; i < num_subscribers; i++) {
        if (subscribers[i].pid == pid) {
            for (uint32_t j = i; j < num_subscribers - 1; j++) {
                subscribers[j] = subscribers[j + 1];
            }
            num_subscribers--;
            break;
        }
    }
    
    spinlock_release(&input_lock);
}

void input_event_enqueue(input_event_t *event) {
    if (!event) return;
    
    spinlock_acquire(&input_lock);
    
    for (uint32_t i = 0; i < num_subscribers; i++) {
        if (subscribers[i].count < INPUT_EVENT_QUEUE_SIZE) {
            uint32_t idx = (subscribers[i].tail + subscribers[i].count) % INPUT_EVENT_QUEUE_SIZE;
            
            input_event_t *dest = &subscribers[i].events[idx];
            dest->type = event->type;
            dest->timestamp = event->timestamp;
            if (event->type == INPUT_EVENT_KEY_PRESS || event->type == INPUT_EVENT_KEY_RELEASE) {
                dest->data.key.scancode = event->data.key.scancode;
                dest->data.key.keycode = event->data.key.keycode;
            } else {
                dest->data.mouse.dx = event->data.mouse.dx;
                dest->data.mouse.dy = event->data.mouse.dy;
                dest->data.mouse.buttons = event->data.mouse.buttons;
            }
            
            subscribers[i].count++;
        }
    }
    
    spinlock_release(&input_lock);
}

int input_event_read(uint32_t pid, input_event_t *event) {
    if (!event) return -1;
    
    spinlock_acquire(&input_lock);
    
    for (uint32_t i = 0; i < num_subscribers; i++) {
        if (subscribers[i].pid == pid) {
            if (subscribers[i].count > 0) {
                input_event_t *src = &subscribers[i].events[subscribers[i].tail];
                event->type = src->type;
                event->timestamp = src->timestamp;
                if (src->type == INPUT_EVENT_KEY_PRESS || src->type == INPUT_EVENT_KEY_RELEASE) {
                    event->data.key.scancode = src->data.key.scancode;
                    event->data.key.keycode = src->data.key.keycode;
                } else {
                    event->data.mouse.dx = src->data.mouse.dx;
                    event->data.mouse.dy = src->data.mouse.dy;
                    event->data.mouse.buttons = src->data.mouse.buttons;
                }
                
                subscribers[i].tail = (subscribers[i].tail + 1) % INPUT_EVENT_QUEUE_SIZE;
                subscribers[i].count--;
                spinlock_release(&input_lock);
                return 0;
            }
            spinlock_release(&input_lock);
            return -1;
        }
    }
    
    spinlock_release(&input_lock);
    return -1;
}
