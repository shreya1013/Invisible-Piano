#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "driver/uart.h"
#include "freertos/semphr.h" // needed for semaphore
#include "driver/timer.h"    // needed for hardware timer
#include "esp_log.h"

static const char *TAG = "invisible_piano";

// I2S pins
#define I2S_NUM I2S_NUM_0
#define I2S_BCLK 27 // Green Wire
#define I2S_LRC 26  // Yellow Wire
#define I2S_DOUT 25 // Brown Wire

// Audio settings
#define SAMPLE_RATE 44100
#define BUFFER_SIZE 512

// UART settings
#define UART_NUM UART_NUM_0
#define UART_BUF_SIZE 1024

// Timer settings
#define TIMER_GROUP TIMER_GROUP_0
#define TIMER_IDX TIMER_0
#define TIMER_DIVIDER 80 // 80 MHz / 80 = 1 MHz (1 tick = 1 microsecond)

// C major scale - first 5 notes (thumb to pinky)
static const float NOTES[5] = {
    261.63f, // C - thumb
    293.66f, // D - index
    329.63f, // E - middle
    349.23f, // F - ring
    392.00f, // G - pinky
};

// Current finger states and BPM
static bool finger_down[5] = {false, false, false, false, false};
static int current_bpm = 60;
static float phase[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

// Semaphore: timer ISR signals audio_task on every beat
static SemaphoreHandle_t beat_semaphore = NULL;

// ============================================================
// TIMER ISR — fires once per beat, wakes audio_task
// interval (us) = 1,000,000 * 60 / BPM
// e.g. 60 BPM -> fires every 1,000,000 us (1 second)
//      120 BPM -> fires every 500,000 us (0.5 seconds)
// ============================================================
static bool IRAM_ATTR timer_isr(void *args)
{
    BaseType_t higher_priority_woken = pdFALSE;
    xSemaphoreGiveFromISR(beat_semaphore, &higher_priority_woken);
    return higher_priority_woken == pdTRUE;
}

// Initialize the hardware timer
static void timer_init_metronome(int bpm)
{
    uint64_t alarm_us = (uint64_t)(1000000ULL * 60 / bpm);

    timer_config_t config = {
        .divider = TIMER_DIVIDER,
        .counter_dir = TIMER_COUNT_UP,
        .counter_en = TIMER_PAUSE,
        .alarm_en = TIMER_ALARM_EN,
        .auto_reload = TIMER_AUTORELOAD_EN, // resets automatically so it keeps firing every beat
    };

    timer_init(TIMER_GROUP, TIMER_IDX, &config);
    timer_set_counter_value(TIMER_GROUP, TIMER_IDX, 0);
    timer_set_alarm_value(TIMER_GROUP, TIMER_IDX, alarm_us);
    timer_enable_intr(TIMER_GROUP, TIMER_IDX);
    timer_isr_callback_add(TIMER_GROUP, TIMER_IDX, timer_isr, NULL, 0);
    timer_start(TIMER_GROUP, TIMER_IDX);

    ESP_LOGI(TAG, "Metronome started: %d BPM, fires every %llu us", bpm, alarm_us);
}

// Set up I2S
static void i2s_init(void)
{
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = BUFFER_SIZE,
        .use_apll = false,
        .tx_desc_auto_clear = true,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_LRC,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM, &pin_config);
    ESP_LOGI(TAG, "I2S initialized");
}

// Parse incoming serial message from Python
// Format: F:10110,BPM:120
static void parse_message(const char *msg)
{
    // Parse finger states
    const char *f_ptr = strstr(msg, "F:");
    if (f_ptr)
    {
        f_ptr += 2;
        for (int i = 0; i < 5; i++)
        {
            if (f_ptr[i] == '1' || f_ptr[i] == '0')
            {
                finger_down[i] = (f_ptr[i] == '1');
            }
        }
    }

    // Parse BPM
    const char *bpm_ptr = strstr(msg, "BPM:");
    if (bpm_ptr)
    {
        bpm_ptr += 4;
        current_bpm = atoi(bpm_ptr);
        if (current_bpm < 40)
            current_bpm = 40;
        if (current_bpm > 200)
            current_bpm = 200;
    }

    ESP_LOGI(TAG, "Fingers: %d%d%d%d%d | BPM: %d",
             finger_down[0], finger_down[1], finger_down[2],
             finger_down[3], finger_down[4], current_bpm);
}

// ============================================================
// AUDIO TASK
// NOW: blocks on beat_semaphore instead of free-running.
// Waits for the timer ISR to fire, then generates one beat
// of audio. This is what makes it real-time — the beat timing
// is enforced by hardware, not by how fast the loop runs.
// ============================================================
static void audio_task(void *arg)
{
    int16_t samples[BUFFER_SIZE * 2]; // stereo so x2
    size_t bytes_written;

    while (1)
    {
        // Block here until the timer ISR signals a beat has elapsed
        xSemaphoreTake(beat_semaphore, portMAX_DELAY);

        // How many samples per beat
        int samples_per_beat = SAMPLE_RATE * 60 / current_bpm;
        int samples_generated = 0;

        while (samples_generated < samples_per_beat)
        {
            int chunk = BUFFER_SIZE;
            if (samples_generated + chunk > samples_per_beat)
            {
                chunk = samples_per_beat - samples_generated;
            }

            // Generate audio samples
            for (int i = 0; i < chunk; i++)
            {
                float sample = 0.0f;
                int active_count = 0;

                // Add sine wave for each finger that is down
                for (int f = 0; f < 5; f++)
                {
                    if (finger_down[f])
                    {
                        float freq = NOTES[f];
                        sample += sinf(phase[f]);
                        phase[f] += 2.0f * M_PI * freq / SAMPLE_RATE;
                        if (phase[f] > 2.0f * M_PI)
                        {
                            phase[f] -= 2.0f * M_PI;
                        }
                        active_count++;
                    }
                }

                // Normalize so multiple notes don't clip
                if (active_count > 0)
                {
                    sample /= active_count;
                }

                // Scale to 16-bit range
                int16_t out = (int16_t)(sample * 16000);

                // Write same value to both left and right channels
                samples[i * 2] = out;
                samples[i * 2 + 1] = out;
            }

            i2s_write(I2S_NUM, samples, chunk * 4, &bytes_written, portMAX_DELAY);
            samples_generated += chunk;
        }
    }
}

// UART task - reads serial messages from Python
static void uart_task(void *arg)
{
    uint8_t data[UART_BUF_SIZE];
    char line[UART_BUF_SIZE];
    int line_pos = 0;

    while (1)
    {
        int len = uart_read_bytes(UART_NUM, data, UART_BUF_SIZE, 20 / portTICK_PERIOD_MS);

        for (int i = 0; i < len; i++)
        {
            char c = (char)data[i];
            if (c == '\n')
            {
                line[line_pos] = '\0';
                if (line_pos > 0)
                {
                    parse_message(line);
                }
                line_pos = 0;
            }
            else if (line_pos < UART_BUF_SIZE - 1)
            {
                line[line_pos++] = c;
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Invisible Piano starting...");

    // Create the beat semaphore BEFORE starting the timer
    beat_semaphore = xSemaphoreCreateBinary();

    // Initialize I2S
    i2s_init();

    // Initialize UART (already set up by default on UART0)
    uart_driver_install(UART_NUM, UART_BUF_SIZE, 0, 0, NULL, 0);

    // Start the metronome timer
    timer_init_metronome(current_bpm);

    // Start audio task on core 1 (dedicated to audio)
    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, 5, NULL, 1);

    // Start UART task on core 0
    xTaskCreatePinnedToCore(uart_task, "uart_task", 4096, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "Tasks started");
}