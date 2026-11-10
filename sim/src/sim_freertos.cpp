// FreeRTOS tasks, semaphores, critical sections and esp_timer on std::thread.
#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "sim_state.h"

struct SimTask {
  std::string name;
  uint32_t stack_depth = 0;
};

namespace {
std::recursive_mutex s_critical;
thread_local SimTask* t_current = nullptr;
SimTask s_main_task{"loopTask", 8192};

/** Thrown by vTaskDelete(NULL) to unwind the calling task's thread. */
struct TaskExit {};
}  // namespace

extern "C" void sim_critical_enter(void) { s_critical.lock(); }
extern "C" void sim_critical_exit(void) { s_critical.unlock(); }

extern "C" BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name,
                                              uint32_t stack_depth, void* param,
                                              UBaseType_t priority, TaskHandle_t* out,
                                              BaseType_t core) {
  (void)priority;
  (void)core;
  auto* task = new SimTask{name ? name : "task", stack_depth};
  if (out != nullptr) *out = task;
  std::thread([fn, param, task]() {
    t_current = task;
    try {
      fn(param);
    } catch (const TaskExit&) {
    } catch (const std::exception& e) {
      fprintf(stderr, "[sim] task %s threw: %s\n", task->name.c_str(), e.what());
    }
  }).detach();
  return pdPASS;
}

extern "C" BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack_depth,
                                  void* param, UBaseType_t priority, TaskHandle_t* out) {
  return xTaskCreatePinnedToCore(fn, name, stack_depth, param, priority, out, tskNO_AFFINITY);
}

extern "C" void vTaskDelete(TaskHandle_t task) {
  if (task == nullptr || task == t_current) {
    if (t_current != nullptr && t_current != &s_main_task) {
      throw TaskExit{};
    }
    return;
  }
  // Deleting another task: std::thread cannot be killed. The firmware only does
  // this for workers it has already told to stop, so let them run out.
}

extern "C" void vTaskDelay(TickType_t ticks) { delay(ticks); }
extern "C" TaskHandle_t xTaskGetCurrentTaskHandle(void) {
  return t_current != nullptr ? t_current : &s_main_task;
}
extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4096; }
extern "C" TickType_t xTaskGetTickCount(void) { return static_cast<TickType_t>(millis()); }
extern "C" const char* pcTaskGetName(TaskHandle_t task) {
  if (task == nullptr) task = xTaskGetCurrentTaskHandle();
  return task->name.c_str();
}
extern "C" BaseType_t xPortGetCoreID(void) { return 1; }

// --- Semaphores: counting semaphore covers mutex (1/1), binary (0/1), counting.
struct SimSemaphore {
  std::mutex m;
  std::condition_variable cv;
  UBaseType_t count;
  UBaseType_t max;
  bool recursive = false;
  std::thread::id owner;
  UBaseType_t depth = 0;
};

namespace {
SimSemaphore* makeSem(UBaseType_t max, UBaseType_t initial) {
  auto* s = new SimSemaphore();
  s->count = initial;
  s->max = max;
  return s;
}
}  // namespace

extern "C" SemaphoreHandle_t xSemaphoreCreateMutex(void) { return makeSem(1, 1); }
extern "C" SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {
  auto* s = makeSem(1, 1);
  s->recursive = true;
  return s;
}
extern "C" SemaphoreHandle_t xSemaphoreCreateBinary(void) { return makeSem(1, 0); }
extern "C" SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max_count, UBaseType_t initial) {
  return makeSem(max_count, initial);
}

extern "C" BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
  if (s == nullptr) return pdFALSE;
  std::unique_lock<std::mutex> lock(s->m);
  if (s->recursive && s->depth > 0 && s->owner == std::this_thread::get_id()) {
    ++s->depth;
    return pdTRUE;
  }
  auto ready = [s] { return s->count > 0; };
  if (ticks == portMAX_DELAY) {
    s->cv.wait(lock, ready);
  } else if (!s->cv.wait_for(lock, std::chrono::milliseconds(ticks), ready)) {
    return pdFALSE;
  }
  --s->count;
  if (s->recursive) {
    s->owner = std::this_thread::get_id();
    s->depth = 1;
  }
  return pdTRUE;
}

extern "C" BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
  if (s == nullptr) return pdFALSE;
  {
    std::lock_guard<std::mutex> lock(s->m);
    if (s->recursive && s->depth > 1) {
      --s->depth;
      return pdTRUE;
    }
    if (s->count >= s->max) return pdFALSE;
    ++s->count;
    s->depth = 0;
  }
  s->cv.notify_one();
  return pdTRUE;
}

extern "C" BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t ticks) {
  return xSemaphoreTake(s, ticks);
}
extern "C" BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s) { return xSemaphoreGive(s); }
extern "C" UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s) {
  if (s == nullptr) return 0;
  std::lock_guard<std::mutex> lock(s->m);
  return s->count;
}
extern "C" void vSemaphoreDelete(SemaphoreHandle_t s) { delete s; }

// --- esp_timer
struct SimTimer {
  esp_timer_create_args_t args;
  std::atomic<bool> running{false};
  std::atomic<uint32_t> generation{0};
};

extern "C" int64_t esp_timer_get_time(void) { return static_cast<int64_t>(micros()); }

extern "C" esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out) {
  auto* t = new SimTimer();
  t->args = *args;
  *out = t;
  return ESP_OK;
}

namespace {
void startTimer(SimTimer* t, uint64_t us, bool periodic) {
  const uint32_t gen = ++t->generation;
  t->running = true;
  std::thread([t, us, periodic, gen]() {
    do {
      std::this_thread::sleep_for(std::chrono::microseconds(us));
      if (!t->running || t->generation != gen || sim::quitRequested()) return;
      t->args.callback(t->args.arg);
    } while (periodic);
    t->running = false;
  }).detach();
}
}  // namespace

extern "C" esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period_us) {
  startTimer(t, period_us, true);
  return ESP_OK;
}
extern "C" esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us) {
  startTimer(t, timeout_us, false);
  return ESP_OK;
}
extern "C" esp_err_t esp_timer_stop(esp_timer_handle_t t) {
  t->running = false;
  ++t->generation;
  return ESP_OK;
}
extern "C" esp_err_t esp_timer_delete(esp_timer_handle_t t) {
  esp_timer_stop(t);
  return ESP_OK;  // leaked on purpose: a sleeping timer thread may still read it
}
