#pragma once

#include <mutex>
#include <thread>
#include <functional>
#include <atomic>
#include <algorithm>
#include <cassert>
#include <map>
#include <sstream>
#include <deque>

extern void marlin_loop();
extern "C" void TIMER0_IRQHandler();
extern "C" void TIMER1_IRQHandler();
extern "C" void SYSTICK_IRQHandler();

constexpr inline uint64_t tickConvertFrequency(std::uint64_t value, std::uint64_t from, std::uint64_t to) {
  return from > to ? value / (from / to) : value * (to / from);
}

struct KernelTimer {
  KernelTimer(std::string name, void (*callback)()) : name(name), isr_function(callback) {}

  bool interrupt(const uint64_t source_count, const uint64_t frequency) {
    return source_count > next_interrupt(frequency);
  }

  uint64_t next_interrupt(const uint64_t source_frequency) {
    return active ? source_offset + tickConvertFrequency(compare, timer_frequency, source_frequency) : std::numeric_limits<uint64_t>::max();
  }

  void initialise(const uint64_t timer_frequency) { this->timer_frequency = timer_frequency; }
  void start(const uint64_t source_count, const uint64_t interrupt_frequency) {
    compare = timer_frequency / interrupt_frequency;
    source_offset = source_count;
  }
  void enable() { active = true; }
  bool enabled() { return active; }
  void disable() { active = false; }

  // in timer frequency
  void set_compare(const uint64_t compare) { this->compare = compare; }
  uint64_t get_compare() { return compare; }
  uint64_t get_count(const uint64_t source_count, const uint64_t source_frequency) { return tickConvertFrequency(source_count - source_offset, source_frequency, timer_frequency); }


  void set_isr(std::string name, void (*callback)()) {
    isr_function = {callback};
    this->name = name;
  }
  void execute() {
    running = true;
    isr_function();
    running = false;
  }

  std::string name;
  bool active = false;
  bool running = false;
  uint64_t compare = 0, source_offset = 0, timer_frequency = 0, priority = 10;
  std::function<void()> isr_function;
};

class Kernel {
public:
  enum TimingMode {
    ISRSTEP
    //REALTIME_SIGNAL
  };

  // ordered highest priority first
  Kernel() : timers({KernelTimer{"Stepper ISR", TIMER0_IRQHandler}, {"Temperate ISR", TIMER1_IRQHandler}, {"SysTick", SYSTICK_IRQHandler}, {"Marlin Loop", marlin_loop}}),
             last_clock_read(clock.now()) {
    timerInit(3, 1000000);
    timers[0].priority = 0;
    timers[1].priority = 5;
    timers[2].priority = 1;
  }

  inline void updateRealtime() {
    auto now = clock.now();
    auto delta = now - last_clock_read;
    uint64_t delta_uint64 = std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count();
    if(delta_uint64 > std::numeric_limits<std::uint64_t>::max() - ONE_BILLION) {
      //printf("rt info: %ld : %f\n", delta_uint64, realtime_scale.load());
      //aparently time can go backwards, thread issue?
      delta_uint64 = 0;
    }
    uint64_t delta_uint64_scaled = delta_uint64 * realtime_scale;
    if (delta_uint64_scaled != 0) {
      last_clock_read = now;
      realtime_nanos += delta_uint64_scaled;
    }
  }

  inline uint64_t getRealtimeTicks() { return nanosToTicks(realtime_nanos); }

  //execute highest priority thread with closest interrupt, return true if something was executed
  bool execute_loop(uint64_t max_end_ticks = std::numeric_limits<uint64_t>::max());
  // if a thread wants to wait, see what should be executed during that wait
  void delayCycles(uint64_t cycles);
  // this was neede for when marlin loops idle waiting for an event with no delays
  void yield();

  //Timers
  inline void disableInterrupts() {
    timers_active = false;
  }

  inline void enableInterrupts() {
    timers_active = true;
  }

  inline void timerInit(uint8_t timer_id, uint32_t rate) {
    if (timer_id < timers.size()) {
       timers[timer_id].timer_frequency = rate;
      // printf("Timer[%d] Initialised( rate: %d )\n", timer_id, rate);
    }
  }

  inline void timerStart(uint8_t timer_id, uint32_t interrupt_frequency) {
    if (timer_id < timers.size()) {
      timers[timer_id].compare = timers[timer_id].timer_frequency / interrupt_frequency;
      timers[timer_id].source_offset = getTicks();
      // printf("Timer[%d] Started( frequency: %d compare: %ld)\n", timer_id, interrupt_frequency, timers[timer_id].compare);
    }
  }

  inline void timerEnable(uint8_t timer_id) {
    if (timer_id < timers.size()) {
      timers[timer_id].active = true;
      // printf("Timer[%d] Enabled\n", timer_id);
    }
  }

  inline bool timerEnabled(uint8_t timer_id) {
    if (timer_id < timers.size())
      return timers[timer_id].active;
    return false;
  }

  inline void timerDisable(uint8_t timer_id) {
    if (timer_id < timers.size()) {
      timers[timer_id].active = false;
      //printf("Timer[%d] Disabled\n", timer_id);
    }
  }

  inline void timerSetCompare(uint8_t timer_id, uint64_t compare) {
    if (timer_id < timers.size()) {
      timers[timer_id].compare = compare;
    }
  }

  inline uint64_t timerGetCount(uint8_t timer_id) {
    if (timer_id < timers.size()) {
      //time must pass here for the stepper isr pulse counter (time + 100ns)
      setTicks(getTicks() + 1 + nanosToTicks(100, timers[timer_id].timer_frequency));
      return timers[timer_id].get_count(getTicks(), frequency);
    }
    return 0;
  }

  inline uint64_t timerGetCompare(uint8_t timer_id) {
    if (timer_id < timers.size())
      return timers[timer_id].compare;
    return 0;
  }

  // Clock
  inline uint64_t getTicks() {
    return ticks.load();
  }

  inline void setTicks(uint64_t new_ticks) {
    ticks.store(new_ticks);
  }

  // constants to reduce risk of typos
  static constexpr uint64_t ONE_BILLION  = 1000'000'000;
  static constexpr uint64_t ONE_MILLION  = 1000'000;
  static constexpr uint64_t ONE_THOUSAND = 1000;

  constexpr static uint64_t nanosToTicks(const uint64_t value, const uint64_t freq) {
    return freq > ONE_BILLION ? value * (freq / ONE_BILLION) : value / (ONE_BILLION / freq);
  }

  inline uint64_t nanosToTicks(uint64_t value) {
    return nanosToTicks(value, frequency);
  }

  constexpr static uint64_t ticksToNanos(const uint64_t value, const uint64_t freq) {
    return freq > ONE_BILLION ? value / (freq / ONE_BILLION) : value * (ONE_BILLION / freq);
  }

  inline uint64_t ticksToNanos(uint64_t value) {
    return ticksToNanos(value, frequency);
  }

  inline uint64_t nanos() {
    setTicks(getTicks() + 1 + nanosToTicks(100)); // Marlin has loops that only break after x ticks, so we need to increment ticks here
    return ticksToNanos(getTicks());
  }

  inline uint64_t micros() {
    return nanos() / ONE_THOUSAND;
  }

  inline uint64_t millis() {
    return nanos() / ONE_MILLION;
  }

  inline double seconds() {
    return ticksToNanos(getTicks()) / double(ONE_BILLION);
  }

  inline void delayNanos(uint64_t ns) {
    delayCycles(nanosToTicks(ns));
  }

  inline void delayMicros(uint64_t micros) {
    delayCycles(nanosToTicks(micros * ONE_THOUSAND));
  }

  inline void delayMillis(uint64_t millis) {
    delayCycles(nanosToTicks(millis * ONE_MILLION));
  }

  inline void delaySeconds(double secs) {
    delayCycles(nanosToTicks(secs * ONE_BILLION));
  }

  bool timers_active = true;
  std::array<KernelTimer, 4> timers;
  std::deque<KernelTimer*> isr_stack;


  bool quit_requested = false;

  std::chrono::steady_clock clock;
  std::chrono::steady_clock::time_point last_clock_read;
  std::atomic_uint64_t isr_timing_error = 0;
  std::atomic<float> realtime_scale = 1.0;
  std::atomic_uint64_t ticks{0};
  uint64_t realtime_nanos = 0;

  static constexpr uint32_t frequency = 100'000'000;
  static bool initialised;
};

extern Kernel kernel;