#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "driver/uart.h"
#include "freertos/semphr.h"
#include "driver/gptimer.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "invisible_piano";

// I2S pins
#define I2S_NUM I2S_NUM_0
#define I2S_BCLK 27
#define I2S_LRC 25
#define I2S_DOUT 26

// Audio settings
#define SAMPLE_RATE 20000
#define BUFFER_SIZE 512
#define NOTE_DURATION_MS 200
#define NOTE_SAMPLES ((SAMPLE_RATE * NOTE_DURATION_MS) / 1000)

// UART settings
#define UART_NUM UART_NUM_0
#define UART_BUF_SIZE 1024

static volatile bool finger_down[5] = {false, false, false, false, false};
static volatile bool audio_enabled = true;
static volatile int current_bpm = 90;
static int global_note_index = 0;
static int16_t note_buffers[5][NOTE_SAMPLES * 2];

static QueueHandle_t uart_event_queue;
static gptimer_handle_t beat_timer = NULL;
static SemaphoreHandle_t beat_semaphore = NULL;

// Scale definitions
static const float SCALES[6][5] = {
    {261.63f, 293.66f, 329.63f, 349.23f, 392.00f}, // C major
    {392.00f, 440.00f, 493.88f, 523.25f, 587.33f}, // G major
    {293.66f, 329.63f, 369.99f, 392.00f, 440.00f}, // D major
    {440.00f, 493.88f, 554.37f, 587.33f, 659.25f}, // A major
    {349.23f, 392.00f, 440.00f, 466.16f, 523.25f}, // F major
    {466.16f, 523.25f, 587.33f, 622.25f, 698.46f}, // Bb major
};

static const char *SCALE_NAMES[6] = {
    "C major", "G major", "D major", "A major", "F major", "Bb major"
};

static float current_notes[5] = {261.63f, 293.66f, 329.63f, 349.23f, 392.00f};

// ============================================================
// INTERRUPT #1 — Hardware timer ISR (gptimer)
// Fires once per beat, signals audio_task via semaphore
// ============================================================
static bool IRAM_ATTR on_alarm_cb(gptimer_handle_t timer,
                                   const gptimer_alarm_event_data_t *edata,
                                   void *user_ctx)
{
    BaseType_t higher_priority_woken = pdFALSE;
    xSemaphoreGiveFromISR(beat_semaphore, &higher_priority_woken);
    return higher_priority_woken == pdTRUE;
}

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

static void generate_note_buffers(void)
{
    for (int i = 0; i < 5; i++) {
        float phase = 0.0f;
        float phase_step = 2.0f * M_PI * current_notes[i] / SAMPLE_RATE;

        for (int j = 0; j < NOTE_SAMPLES; j++) {
            float sample = sinf(phase);
            float envelope = 1.0f;
            if (j > NOTE_SAMPLES * 0.8f) {
                envelope = (float)(NOTE_SAMPLES - j) / (NOTE_SAMPLES * 0.2f);
            }
            int16_t out = (int16_t)(sample * envelope * 16000);
            note_buffers[i][j * 2]     = out;
            note_buffers[i][j * 2 + 1] = out;
            phase += phase_step;
            if (phase > 2.0f * M_PI) phase -= 2.0f * M_PI;
        }
    }
}

static void update_bpm_timer(int bpm)
{
    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count = (uint64_t)(60000000ULL / bpm),
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_stop(beat_timer);
    gptimer_set_alarm_action(beat_timer, &alarm_cfg);
    gptimer_start(beat_timer);
    ESP_LOGI(TAG, "BPM updated to %d", bpm);
}

static void parse_message(const char *msg)
{
    // ============================================================
    // INTERRUPT #5 — No hands detected
    // NOHANDS message disables audio output
    // ============================================================
    if (strncmp(msg, "NOHANDS", 7) == 0) {
        if (audio_enabled) {
            audio_enabled = false;
            i2s_zero_dma_buffer(I2S_NUM);
            ESP_LOGI(TAG, "No hands detected - audio off");
        }
        return;
    }

    audio_enabled = true;

    // ============================================================
    // INTERRUPT #2 — Finger state change
    // Triggers note on or note off when finger state changes
    // ============================================================
    const char *f_ptr = strstr(msg, "F:");
    if (f_ptr) {
        f_ptr += 2;
        for (int i = 0; i < 5; i++) {
            if (f_ptr[i] == '1' || f_ptr[i] == '0') {
                bool new_state = (f_ptr[i] == '1');
                if (new_state != finger_down[i]) {
                    finger_down[i] = new_state;
                    ESP_LOGI(TAG, "Finger %d %s (%.2f Hz)",
                             i, new_state ? "DOWN (note on)" : "UP (note off)",
                             current_notes[i]);
                }
            }
        }
    }

    // ============================================================
    // INTERRUPT #3 — BPM threshold crossed
    // Updates timer interval when BPM changes significantly
    // ============================================================
    const char *bpm_ptr = strstr(msg, "BPM:");
    if (bpm_ptr) {
        bpm_ptr += 4;
        int new_bpm = atoi(bpm_ptr);
        new_bpm = (new_bpm < 40) ? 40 : (new_bpm > 200) ? 200 : new_bpm;

        if (abs(new_bpm - current_bpm) > 2) {
            current_bpm = new_bpm;
            update_bpm_timer(current_bpm);
        }
    }

    // ============================================================
    // INTERRUPT #4 — Key change
    // Changes list of note frequencies when user changes key in GUI
    // ============================================================
    const char *key_ptr = strstr(msg, "KEY:");
    if (key_ptr) {
        key_ptr += 4;
        for (int s = 0; s < 6; s++) {
            if (strstr(key_ptr, SCALE_NAMES[s]) != NULL) {
                for (int n = 0; n < 5; n++) {
                    current_notes[n] = SCALES[s][n];
                }
                generate_note_buffers();
                ESP_LOGI(TAG, "Scale changed to: %s", SCALE_NAMES[s]);
                break;
            }
        }
    }

    ESP_LOGI(TAG, "Fingers: %d%d%d%d%d | BPM: %d",
             (int)finger_down[0], (int)finger_down[1], (int)finger_down[2],
             (int)finger_down[3], (int)finger_down[4], current_bpm);
}

static void uart_task(void *arg)
{
    uart_event_t event;
    uint8_t data[UART_BUF_SIZE];
    char line[UART_BUF_SIZE];
    int line_pos = 0;

    while (1) {
        if (xQueueReceive(uart_event_queue, &event, portMAX_DELAY)) {
            if (event.type == UART_DATA) {
                int len = uart_read_bytes(UART_NUM, data, event.size,
                                          pdMS_TO_TICKS(20));
                for (int i = 0; i < len; i++) {
                    char c = (char)data[i];
                    if (c == '\n') {
                        line[line_pos] = '\0';
                        if (line_pos > 0) {
                            parse_message(line);
                        }
                        line_pos = 0;
                    } else if (line_pos < UART_BUF_SIZE - 1) {
                        line[line_pos++] = c;
                    }
                }
            } else if (event.type == UART_FIFO_OVF ||
                       event.type == UART_BUFFER_FULL) {
                uart_flush_input(UART_NUM);
                xQueueReset(uart_event_queue);
                ESP_LOGW(TAG, "UART overflow - buffer flushed");
            }
        }
    }
}

static void audio_task(void *arg)
{
    size_t bytes_written;

    while (1) {
        // Interrupt #4 — silence when no hands
        if (!audio_enabled) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Build list of active fingers
        int active_fingers[5];
        int active_count = 0;
        for (int f = 0; f < 5; f++) {
            if (finger_down[f]) {
                active_fingers[active_count++] = f;
            }
        }

        if (active_count == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Wait for hardware timer ISR beat signal (Interrupt #1)
        xSemaphoreTake(beat_semaphore, portMAX_DELAY);

        // Flush extra beats that queued while busy
        while (xSemaphoreTake(beat_semaphore, 0) == pdTRUE) {}

        ESP_LOGI(TAG, "Timer Fired");

        // Advance through active notes
        int finger_index = active_fingers[global_note_index % active_count];
        global_note_index++;
        if (global_note_index > 1000000) global_note_index = 0;

        // Play pre-generated note buffer
        int total_bytes = NOTE_SAMPLES * 2 * sizeof(int16_t);
        uint8_t *buf = (uint8_t *)note_buffers[finger_index];
        int written = 0;
        while (written < total_bytes && audio_enabled) {
            int chunk = 512;
            if (written + chunk > total_bytes) chunk = total_bytes - written;
            i2s_write(I2S_NUM, buf + written, chunk, &bytes_written, portMAX_DELAY);
            written += bytes_written;
        }
        if (!audio_enabled) {
            i2s_zero_dma_buffer(I2S_NUM);
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Invisible Piano starting...");

    beat_semaphore = xSemaphoreCreateBinary();

    // Configure gptimer (Interrupt #1)
    gptimer_config_t config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
        .intr_priority = 3,
    };
    gptimer_new_timer(&config, &beat_timer);

    gptimer_event_callbacks_t cbs = {
        .on_alarm = on_alarm_cb,
    };
    gptimer_register_event_callbacks(beat_timer, &cbs, NULL);

    gptimer_alarm_config_t alarm_config = {
        .alarm_count = (uint64_t)(60000000ULL / current_bpm),
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_set_alarm_action(beat_timer, &alarm_config);
    gptimer_enable(beat_timer);
    gptimer_start(beat_timer);

    // UART with event queue (feeds Interrupts #2, #3, #4, #5)
    uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, 0, 20,
                        &uart_event_queue, 0);

    i2s_init();
    generate_note_buffers();

    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(uart_task,  "uart_task",  4096, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "Tasks started");
}
