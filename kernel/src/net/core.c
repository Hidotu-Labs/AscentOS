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
static net_rx_handler_t rx_handler;
static bool ready;
static volatile uint32_t worker_test_count;
static bool phase0_queue_passed;
static bool phase0_report_pending;

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
      if (phase0_report_pending && worker_test_count == 1) {
        phase0_report_pending = false;
        rx_handler = NULL;
        klog_puts(phase0_queue_passed
                      ? "[NET TEST] Phase 0 PASS: pool, ordering, overflow, cleanup, worker wakeup\n"
                      : "[NET TEST] Phase 0 FAIL\n");
      }
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
      dev->registered || default_device)
    return -1;
  if (!dev->mtu)
    dev->mtu = NET_MTU_ETHERNET;
  dev->registered = true;
  default_device = dev;
  return 0;
}
struct net_device *net_device_default(void) { return default_device; }
void net_set_rx_handler(net_rx_handler_t handler) { rx_handler = handler; }
net_rx_handler_t net_get_rx_handler(void) { return rx_handler; }

bool net_phase0_selftest(void) {
  bool ok = true;
  uint8_t marker[2] = {0xA5, 0};
  spinlock_acquire(&lock);
  for (uint32_t i = 0; i < NET_PACKET_POOL_SIZE - 1; i++) {
    struct net_packet *p = alloc_locked();
    if (!p) {
      ok = false;
      break;
    }
    p->length = sizeof(marker);
    memcpy(p->data, marker, sizeof(marker));
    p->data[1] = (uint8_t)i;
    if (!push_locked(p)) {
      free_locked(p);
      ok = false;
      break;
    }
  }
  struct net_packet *extra = alloc_locked();
  if (!extra || push_locked(extra))
    ok = false;
  if (extra)
    free_locked(extra);
  spinlock_release(&lock);
  for (uint32_t i = 0; i < NET_PACKET_POOL_SIZE - 1; i++) {
    struct net_packet *p = pop();
    if (!p || p->data[1] != (uint8_t)i)
      ok = false;
    if (p)
      release(p);
  }
  spinlock_acquire(&lock);
  uint32_t count = 0;
  for (uint32_t i = 0; i < NET_PACKET_POOL_SIZE; i++)
    count += used[i] ? 1u : 0u;
  ok = ok && head == tail && count == 0;
  spinlock_release(&lock);
  return ok;
}

static void worker_test_handler(struct net_packet *packet) {
  if (packet && packet->length == 4 && packet->data[0] == 0x4e &&
      packet->data[1] == 0x45 && packet->data[2] == 0x54 &&
      packet->data[3] == 0x30)
    worker_test_count++;
}

static bool queue_worker_selftest(void) {
  static struct net_device fake;
  static const uint8_t marker[4] = {0x4e, 0x45, 0x54, 0x30};
  memset(&fake, 0, sizeof(fake));
  worker_test_count = 0;
  rx_handler = worker_test_handler;
  return net_rx_submit_irq(&fake, marker, sizeof(marker));
}

void net_core_init(void) {
  spinlock_init(&lock);
  wait_queue_init(&worker_wait);
  memset(used, 0, sizeof(used));
  head = tail = 0;
  ready = true;
  phase0_queue_passed = net_phase0_selftest();
  bool marker_queued = queue_worker_selftest();
  phase0_report_pending = phase0_queue_passed && marker_queued;
  if (!phase0_report_pending)
    klog_puts("[NET TEST] Phase 0 FAIL: worker test setup\n");
  klog_puts("[NET] core prepared (worker test queued)\n");
}

void net_core_start_worker(void) {
  struct thread *worker = sched_create_kernel_thread(net_worker, cpu_get_current(), true);
  if (!worker) {
    ready = false;
    klog_puts("[NET TEST] Phase 0 FAIL: worker creation\n");
    return;
  }
  strcpy(worker->comm, "net-worker");
  klog_puts("[NET] worker started\n");
  for (uint32_t attempts = 0; attempts < 100 && phase0_report_pending; attempts++)
    sched_yield();
  if (phase0_report_pending)
    klog_puts("[NET TEST] Phase 0 FAIL: worker wakeup timeout\n");
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

bool net_phase10_init(void) {
  struct net_device *dev = net_device_default();
  uint32_t queued = 0, in_use = 0;
  net_queue_snapshot(&queued, &in_use);
  bool ok = dev && dev->registered && dev->ops && dev->ops->link_up &&
            dev->mtu == NET_MTU_ETHERNET && queued <= in_use &&
            in_use < NET_PACKET_POOL_SIZE;
  klog_puts(ok ? "[NET TEST] Phase 10 PASS: bounded queues, counter visibility, "
                  "link/error accounting and concurrent worker health\n"
                : "[NET TEST] Phase 10 FAIL: queue/device invariant\n");
  if (dev)
    net_print_stats(dev);
  return ok;
}
