#include "net/core.h"
#include "apic/lapic_timer.h"
#include "console/klog.h"
#include "lib/string.h"
#include "lock/spinlock.h"
#include "net/tcp.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "smp/cpu.h"

static struct net_packet pool[NET_PACKET_POOL_SIZE];
static bool used[NET_PACKET_POOL_SIZE];
static struct net_packet *queue[NET_PACKET_POOL_SIZE];
static uint32_t head, tail;
static spinlock_t lock = SPINLOCK_INIT;
static wait_queue_t worker_wait;
static struct net_device *default_device;
static struct net_device *devices[NET_DEVICE_MAX];
static net_rx_handler_t rx_handler;
static bool ready;

static struct net_packet *alloc_locked(void) {
  for (uint32_t i = 0; i < NET_PACKET_POOL_SIZE; i++)
    if (!used[i]) {
      used[i] = true;
      return &pool[i];
    }
  return NULL;
}
static void free_locked(struct net_packet *p) {
  if (p >= pool && p < pool + NET_PACKET_POOL_SIZE)
    used[(size_t)(p - pool)] = false;
}
static bool push_locked(struct net_packet *p) {
  uint32_t next = (head + 1) % NET_PACKET_POOL_SIZE;
  if (next == tail)
    return false;
  queue[head] = p;
  head = next;
  return true;
}
static struct net_packet *pop(void) {
  spinlock_acquire(&lock);
  if (tail == head) {
    spinlock_release(&lock);
    return NULL;
  }
  struct net_packet *p = queue[tail];
  tail = (tail + 1) % NET_PACKET_POOL_SIZE;
  spinlock_release(&lock);
  return p;
}
static void release(struct net_packet *p) {
  spinlock_acquire(&lock);
  free_locked(p);
  spinlock_release(&lock);
}

static void net_worker(void) {
  struct thread *self = sched_get_current();
  wait_queue_entry_t entry = {.thread = self, .next = NULL};
  for (;;) {
    struct net_packet *p;
    while ((p = pop()) != NULL) {
      if (rx_handler)
        rx_handler(p);
      release(p);
    }
    tcp_timer_tick(lapic_timer_get_ticks());
    wait_queue_add(&worker_wait, &entry);
    spinlock_acquire(&lock);
    bool empty = head == tail;
    if (empty)
      self->state = THREAD_BLOCKED;
    spinlock_release(&lock);
    if (!empty)
      wait_queue_wake_one(&worker_wait);
    if (empty)
      self->wakeup_ticks = lapic_timer_get_ticks() + 100;
    sched_yield();
    self->wakeup_ticks = 0;
    wait_queue_remove(&worker_wait, &entry);
  }
}

bool net_rx_submit_irq(struct net_device *dev, const void *frame, size_t len) {
  if (!ready || !dev || !frame || !len || len > NET_FRAME_MAX) {
    if (dev)
      dev->stats.rx_errors++;
    return false;
  }
  spinlock_acquire(&lock);
  struct net_packet *p = alloc_locked();
  if (!p) {
    dev->stats.rx_dropped++;
    dev->stats.queue_full++;
    spinlock_release(&lock);
    return false;
  }
  p->device = dev;
  p->length = (uint16_t)len;
  memcpy(p->data, frame, len);
  if (!push_locked(p)) {
    free_locked(p);
    dev->stats.rx_dropped++;
    dev->stats.queue_full++;
    spinlock_release(&lock);
    return false;
  }
  dev->stats.rx_packets++;
  dev->stats.rx_bytes += len;
  spinlock_release(&lock);
  wait_queue_wake_one(&worker_wait);
  return true;
}

int net_device_register(struct net_device *dev) {
  if (!ready || !dev || !dev->ops || !dev->ops->link_up || !dev->ops->stop ||
      dev->registered || !dev->name[0])
    return -1;
  int free_slot = -1;
  for (int i = 0; i < NET_DEVICE_MAX; i++) {
    if (devices[i] && strcmp(devices[i]->name, dev->name) == 0)
      return -1;
    if (!devices[i] && free_slot < 0)
      free_slot = i;
  }
  if (free_slot < 0)
    return -1;
  if (!dev->mtu)
    dev->mtu = NET_MTU_ETHERNET;
  dev->registered = true;
  devices[free_slot] = dev;
  if (!default_device)
    default_device = dev;
  return 0;
}

int net_device_unregister(struct net_device *dev) {
  if (!dev || !dev->registered)
    return -1;
  for (int i = 0; i < NET_DEVICE_MAX; i++)
    if (devices[i] == dev)
      devices[i] = NULL;
  if (default_device == dev) {
    default_device = NULL;
    for (int i = 0; i < NET_DEVICE_MAX; i++)
      if (devices[i]) {
        default_device = devices[i];
        break;
      }
  }
  dev->registered = false;
  return 0;
}

struct net_device *net_device_default(void) { return default_device; }

struct net_device *net_device_find(const char *name) {
  if (!name)
    return NULL;
  for (int i = 0; i < NET_DEVICE_MAX; i++)
    if (devices[i] && strcmp(devices[i]->name, name) == 0)
      return devices[i];
  return NULL;
}
void net_set_rx_handler(net_rx_handler_t handler) { rx_handler = handler; }
net_rx_handler_t net_get_rx_handler(void) { return rx_handler; }

void net_core_init(void) {
  spinlock_init(&lock);
  wait_queue_init(&worker_wait);
  memset(used, 0, sizeof(used));
  memset(devices, 0, sizeof(devices));
  default_device = NULL;
  head = tail = 0;
  ready = true;
  klog_puts("[NET] core prepared\n");
}

void net_core_start_worker(void) {
  struct thread *worker = sched_create_kernel_thread(net_worker, cpu_get_current(), true);
  if (!worker) {
    ready = false;
    klog_puts("[NET] worker creation failed\n");
    return;
  }
  strcpy(worker->comm, "net-worker");
  klog_puts("[NET] worker started\n");
}

void net_print_stats(const struct net_device *dev) {
  if (!dev)
    return;
  klog_puts("[NET] rx=");
  klog_uint64(dev->stats.rx_packets);
  klog_puts(" tx=");
  klog_uint64(dev->stats.tx_packets);
  klog_puts(" drop=");
  klog_uint64(dev->stats.rx_dropped + dev->stats.tx_dropped);
  klog_puts(" err=");
  klog_uint64(dev->stats.rx_errors + dev->stats.tx_errors);
  klog_puts(" irq=");
  klog_uint64(dev->stats.interrupts);
  klog_puts(" reset=");
  klog_uint64(dev->stats.resets);
  klog_puts(" qfull=");
  klog_uint64(dev->stats.queue_full);
  klog_puts(" linkchg=");
  klog_uint64(dev->stats.link_changes);
  klog_puts(" rxovf=");
  klog_uint64(dev->stats.rx_overflows);
  klog_puts(" txunderrun=");
  klog_uint64(dev->stats.tx_underruns);
  klog_puts("\n");
}

void net_queue_snapshot(uint32_t *queued, uint32_t *in_use) {
  uint32_t allocated = 0;
  spinlock_acquire(&lock);
  for (uint32_t i = 0; i < NET_PACKET_POOL_SIZE; i++)
    allocated += used[i] ? 1u : 0u;
  if (queued)
    *queued = (head + NET_PACKET_POOL_SIZE - tail) % NET_PACKET_POOL_SIZE;
  if (in_use)
    *in_use = allocated;
  spinlock_release(&lock);
}
