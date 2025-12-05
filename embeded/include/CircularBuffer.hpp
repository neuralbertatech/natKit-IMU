#pragma once

#include <libnatkit-core.hpp>

#include <array>
#include <optional>

#ifdef USE_FREE_RTOS_LOCKS
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <mutex>
#endif

template <typename T, size_t TSize = 512>
class CircularBuffer {
    static_assert(TSize > 0, "CircularBuffer size must be greater than zero.");

    #ifdef USE_FREE_RTOS_LOCKS
    SemaphoreHandle_t mutex;
    #else
    std::mutex mutex{};
    #endif // USE_FREE_RTOS_LOCKS

    std::array<T, TSize> buffer;
    size_t head{};
    size_t tail{};
    bool is_full{};

private:
    bool unsafeIsEmpty() const {
        return (!is_full && (head == tail));
    }

    bool unsafeIsFull() const {
        return is_full;
    }

public:
    explicit CircularBuffer() {
        #ifdef USE_FREE_RTOS_LOCKS
        mutex = xSemaphoreCreateMutex();
        assert(mutex != nullptr);
        #endif // USE_FREE_RTOS_LOCKS
    }

    CircularBuffer(const CircularBuffer&) = delete;
    CircularBuffer& operator=(const CircularBuffer&) = delete;
    CircularBuffer(CircularBuffer&&) = delete;
    CircularBuffer& operator=(CircularBuffer&&) = delete;

    ~CircularBuffer() {
        #ifdef USE_FREE_RTOS_LOCKS
        if (mutex) {
            vSemaphoreDelete(mutex);
        }
        #endif // USE_FREE_RTOS_LOCKS
    }

    void push(T item) {
        #ifdef USE_FREE_RTOS_LOCKS
        xSemaphoreTake(mutex, portMAX_DELAY);
        #else
        std::lock_guard<std::mutex> guard(mutex);
        #endif // USE_FREE_RTOS_LOCKS

        buffer[head] = item;
        if (is_full) {
            tail = (tail + 1) % TSize;
        }
        head = (head + 1) % TSize;
        is_full = head == tail;
        
        #ifdef USE_FREE_RTOS_LOCKS
        xSemaphoreGive(mutex);
        #endif // USE_FREE_RTOS_LOCKS
    }

    std::unique_ptr<T> pop() {
        #ifdef USE_FREE_RTOS_LOCKS
        xSemaphoreTake(mutex, portMAX_DELAY);
        #else
        std::lock_guard<std::mutex> guard(mutex);
        #endif // USE_FREE_RTOS_LOCKS
        if (unsafeIsEmpty()) {
            #ifdef USE_FREE_RTOS_LOCKS
            xSemaphoreGive(mutex);
            #endif // USE_FREE_RTOS_LOCKS
            return {};
        } else {
            const auto value = buffer[tail];
            is_full = false;
            tail = (tail + 1) % TSize;
            
            #ifdef USE_FREE_RTOS_LOCKS
            xSemaphoreGive(mutex);
            #endif // USE_FREE_RTOS_LOCKS

            T* value_ptr = new T(value);
            std::unique_ptr<T> unique_value(value_ptr);
            return unique_value;
        }
    }

    std::unique_ptr<T> peek() {
        #ifdef USE_FREE_RTOS_LOCKS
        xSemaphoreTake(mutex, portMAX_DELAY);
        #else
        std::lock_guard<std::mutex> guard(mutex);
        #endif // USE_FREE_RTOS_LOCKS
        if (unsafeIsEmpty()) {
            #ifdef USE_FREE_RTOS_LOCKS
            xSemaphoreGive(mutex);
            #endif // USE_FREE_RTOS_LOCKS
            return {};
        } else {
            const auto val = buffer[tail];
            #ifdef USE_FREE_RTOS_LOCKS
            xSemaphoreGive(mutex);
            #endif // USE_FREE_RTOS_LOCKS
            T* value_ptr = new T(val);
            std::unique_ptr<T> unique_value(value_ptr);
            return unique_value;
        }
    }

    std::unique_ptr<T> peekNext() {
        #ifdef USE_FREE_RTOS_LOCKS
        xSemaphoreTake(mutex, portMAX_DELAY);
        #else
        std::lock_guard<std::mutex> guard(mutex);
        #endif // USE_FREE_RTOS_LOCKS
        const auto next_tail = (tail + 1) % TSize;
        if (unsafeIsEmpty() || (!is_full && (head == next_tail))) {
            xSemaphoreGive(mutex);
            #ifdef USE_FREE_RTOS_LOCKS
            xSemaphoreGive(mutex);
            #endif // USE_FREE_RTOS_LOCKS
            return {};
        } else {
            const auto val = buffer[next_tail];
            #ifdef USE_FREE_RTOS_LOCKS
            xSemaphoreGive(mutex);
            #endif // USE_FREE_RTOS_LOCKS
            T* value_ptr = new T(val);
            std::unique_ptr<T> unique_value(value_ptr);
            return unique_value;
        }
    }

    void reset() {
        #ifdef USE_FREE_RTOS_LOCKS
        xSemaphoreTake(mutex, portMAX_DELAY);
        #else
        std::lock_guard<std::mutex> guard(mutex);
        #endif // USE_FREE_RTOS_LOCKS
        head = tail;
        is_full = false;
        
        #ifdef USE_FREE_RTOS_LOCKS
        xSemaphoreGive(mutex);
        #endif // USE_FREE_RTOS_LOCKS
    }

    bool isEmpty() const {
        #ifdef USE_FREE_RTOS_LOCKS
        xSemaphoreTake(mutex, portMAX_DELAY);
        #else
        std::lock_guard<std::mutex> guard(mutex);
        #endif // USE_FREE_RTOS_LOCKS
        const auto val = unsafeIsEmpty();
        #ifdef USE_FREE_RTOS_LOCKS
        xSemaphoreGive(mutex);
        #endif // USE_FREE_RTOS_LOCKS
        return val;
    }

    bool isFull() const {
        #ifdef USE_FREE_RTOS_LOCKS
        xSemaphoreTake(mutex, portMAX_DELAY);
        #else
        std::lock_guard<std::mutex> guard(mutex);
        #endif // USE_FREE_RTOS_LOCKS
        const auto val = unsafeIsFull();
        #ifdef USE_FREE_RTOS_LOCKS
        xSemaphoreGive(mutex);
        #endif // USE_FREE_RTOS_LOCKS
        return val;
    }
};