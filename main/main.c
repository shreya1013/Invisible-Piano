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

// Test
#define TEST_DELAY_MS 5000 // pause between each test step

static volatile bool timer_flag = false;
static volatile bool finger_down[5] = {false, false, false, false, true};
static volatile bool audio_enabled = true;
static volatile int current_bpm = 90;
static int global_note_index = 0;
static int16_t note_buffers[5][NOTE_SAMPLES * 2];

static QueueHandle_t uart_event_queue;

static gptimer_handle_t beat_timer = NULL;

static SemaphoreHandle_t beat_semaphore = NULL;

static bool IRAM_ATTR on_alarm_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    BaseType_t higher_priority_woken = pdFALSE;
    xSemaphoreGiveFromISR(beat_semaphore, &higher_priority_woken);
    return higher_priority_woken == pdTRUE;
}

// C major scale - first 5 notes (thumb to pinky)
static const float NOTES[5] = {
    261.63f, // C - thumb
    293.66f, // D - index
    329.63f, // E - middle
    349.23f, // F - ring
    392.00f, // G - pinky
};

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
    for (int i = 0; i < 5; i++)
    {
        float phase = 0.0f;
        float phase_step = 2.0f * M_PI * NOTES[i] / SAMPLE_RATE;

        for (int j = 0; j < NOTE_SAMPLES; j++)
        {
            float sample = sinf(phase);

            float envelope = 1.0f;
            if (j > NOTE_SAMPLES * 0.8f)
            {
                envelope = (float)(NOTE_SAMPLES - j) / (NOTE_SAMPLES * 0.2f);
            }

            int16_t out = (int16_t)(sample * envelope * 16000);

            note_buffers[i][j * 2] = out;
            note_buffers[i][j * 2 + 1] = out;

            phase += phase_step;
            if (phase > 2.0f * M_PI)
            {
                phase -= 2.0f * M_PI;
            }
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
    ESP_LOGI(TAG, "BPM updated to %d (period %11u microseconds)", bpm,
             (uint64_t)(60000000ULL / bpm));
}

static void parse_message(const char *msg)
{
    if (strncmp(msg, "NOHANDS", 7) == 0)
    {
        if (audio_enabled)
        {
            audio_enabled = false;
            i2s_zero_dma_buffer(I2S_NUM);
            ESP_LOGI(TAG, "No hands detected - audio off");
        }
        return;
    }

    audio_enabled = true;

    const char *f_ptr = strstr(msg, "F:");
    if (f_ptr)
    {
        f_ptr += 2;
        bool changed = false;
        for (int i = 0; i < 5; i++)
        {
            if (f_ptr[i] == '1' || f_ptr[i] == '0')
            {
                bool new_state = (f_ptr[i] == '1');
                if (new_state != finger_down[i])
                {
                    finger_down[i] = new_state;
                    changed = true;

                    ESP_LOGI(TAG, "Finger %d %s (%.2f Hz)",
                             i, new_state ? "DOWN (note on)" : "UP (note off)",
                             NOTES[i]);
                }
            }
        }

        if (changed)
        {
            global_note_index = 0;
        }
    }

    const char *bpm_ptr = strstr(msg, "BPM:");
    if (bpm_ptr)
    {
        bpm_ptr += 4;
        int new_bpm = atoi(bpm_ptr);
        new_bpm = (new_bpm < 40) ? 40 : (new_bpm > 200) ? 200
                                                        : new_bpm;

        if (abs(new_bpm - current_bpm) > 2)
        {
            current_bpm = new_bpm;
            update_bpm_timer(current_bpm);
        }
    }
}

static void uart_task(void *arg)
{
    uart_event_t event;
    uint8_t data[UART_BUF_SIZE];
    char line[UART_BUF_SIZE];
    int line_pos = 0;

    while (1)
    {

        if (xQueueReceive(uart_event_queue, &event, portMAX_DELAY))
        {

            if (event.type == UART_DATA)
            {
                int len = uart_read_bytes(UART_NUM, data, event.size,
                                          pdMS_TO_TICKS(20));

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
            else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL)
            {
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

    while (1)
    {

        if (!audio_enabled)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Build list of active notes
        int active_fingers[5];
        int active_count = 0;
        for (int f = 0; f < 5; f++)
        {
            if (finger_down[f])
            {
                active_fingers[active_count++] = f;
            }
        }

        if (active_count == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        xSemaphoreTake(beat_semaphore, portMAX_DELAY);
        ESP_LOGI(TAG, "Timer Fired");
        int finger_index = active_fingers[global_note_index % active_count];
        global_note_index++;

        i2s_write(
            I2S_NUM,
            note_buffers[finger_index],
            NOTE_SAMPLES * 2 * sizeof(int16_t),
            &bytes_written,
            portMAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void test_task(void *arg)
{
    ESP_LOGI(TAG, "\n\n");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  INVISIBLE PIANO INTERRUPT TEST SUITE");
    ESP_LOGI(TAG, "========================================\n");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 1: Timer firing (Interrupt 1)
    // The gptimer ISR and beat_semaphore are already running from app_main.
    // Just let it run and watch "Timer Fired" appear at 90 BPM cadence.
    // ─────────────────────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "TEST 1: Timer firing at default %d BPM", current_bpm);
    ESP_LOGI(TAG, "Watch for 'Timer Fired' repeating every ~%llu ms",
             60000ULL / current_bpm);
    ESP_LOGI(TAG, "----------------------------------------");
    vTaskDelay(pdMS_TO_TICKS(5000)); // watch for 5 seconds

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 2: Finger state changes (Interrupt 2)
    // Directly toggle finger_down[] the same way parse_message() would.
    // Expected: "Finger X DOWN/UP (xxx.xx Hz)" log per change.
    // ─────────────────────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "TEST 2: Finger state changes (Interrupt 2)");
    ESP_LOGI(TAG, "----------------------------------------");

    // Step 1: only thumb down
    ESP_LOGI(TAG, ">> Setting only thumb down (F:10000)");
    finger_down[0] = true;
    finger_down[1] = false;
    finger_down[2] = false;
    finger_down[3] = false;
    finger_down[4] = false;
    global_note_index = 0;
    for (int i = 0; i < 5; i++)
        ESP_LOGI(TAG, "  Finger %d %s (%.2f Hz)",
                 i, finger_down[i] ? "DOWN (note on)" : "UP  (note off)", NOTES[i]);
    vTaskDelay(pdMS_TO_TICKS(TEST_DELAY_MS));

    // Step 2: thumb + index
    ESP_LOGI(TAG, ">> Adding index finger (F:11000)");
    finger_down[1] = true;
    global_note_index = 0;
    for (int i = 0; i < 5; i++)
        ESP_LOGI(TAG, "  Finger %d %s (%.2f Hz)",
                 i, finger_down[i] ? "DOWN (note on)" : "UP  (note off)", NOTES[i]);
    vTaskDelay(pdMS_TO_TICKS(TEST_DELAY_MS));

    // Step 3: thumb + index + middle
    ESP_LOGI(TAG, ">> Adding middle finger (F:11100)");
    finger_down[2] = true;
    global_note_index = 0;
    for (int i = 0; i < 5; i++)
        ESP_LOGI(TAG, "  Finger %d %s (%.2f Hz)",
                 i, finger_down[i] ? "DOWN (note on)" : "UP  (note off)", NOTES[i]);
    vTaskDelay(pdMS_TO_TICKS(TEST_DELAY_MS));

    // Step 4: only middle — thumb and index release
    ESP_LOGI(TAG, ">> Releasing thumb and index (F:00100)");
    finger_down[0] = false;
    finger_down[1] = false;
    global_note_index = 0;
    for (int i = 0; i < 5; i++)
        ESP_LOGI(TAG, "  Finger %d %s (%.2f Hz)",
                 i, finger_down[i] ? "DOWN (note on)" : "UP  (note off)", NOTES[i]);
    vTaskDelay(pdMS_TO_TICKS(TEST_DELAY_MS));

    // Step 5: all fingers up
    ESP_LOGI(TAG, ">> All fingers up (F:00000) - audio_task should idle");
    for (int i = 0; i < 5; i++)
        finger_down[i] = false;
    global_note_index = 0;
    vTaskDelay(pdMS_TO_TICKS(TEST_DELAY_MS));

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 3: BPM threshold crossing (Interrupt 3)
    // Calls update_bpm_timer() directly, same as parse_message() would.
    // Expected: log fires only when |new - current| > 2 (hysteresis check).
    // ─────────────────────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "TEST 3: BPM threshold crossing (Interrupt 3)");
    ESP_LOGI(TAG, "----------------------------------------");

    // Restore some fingers so audio keeps playing during BPM test
    finger_down[0] = true;
    finger_down[2] = true;
    finger_down[4] = true;
    global_note_index = 0;

    // Within hysteresis — should NOT trigger update_bpm_timer
    ESP_LOGI(TAG, ">> Sending BPM 91 (within ±2 of 90) — expect NO timer update");
    int test_bpm = 91;
    if (abs(test_bpm - current_bpm) > 2)
    {
        current_bpm = test_bpm;
        update_bpm_timer(current_bpm);
    }
    else
    {
        ESP_LOGI(TAG, "  Hysteresis held — timer NOT updated (correct)");
    }
    vTaskDelay(pdMS_TO_TICKS(TEST_DELAY_MS));

    // Crosses threshold — SHOULD trigger
    ESP_LOGI(TAG, ">> Sending BPM 120 — expect timer update");
    test_bpm = 120;
    if (abs(test_bpm - current_bpm) > 2)
    {
        current_bpm = test_bpm;
        update_bpm_timer(current_bpm);
    }
    vTaskDelay(pdMS_TO_TICKS(TEST_DELAY_MS));

    // Jump down
    ESP_LOGI(TAG, ">> Sending BPM 60 — expect timer update");
    test_bpm = 60;
    if (abs(test_bpm - current_bpm) > 2)
    {
        current_bpm = test_bpm;
        update_bpm_timer(current_bpm);
    }
    vTaskDelay(pdMS_TO_TICKS(TEST_DELAY_MS));

    // Minimum
    ESP_LOGI(TAG, ">> Sending BPM 40 (minimum) — expect timer update");
    test_bpm = 40;
    if (abs(test_bpm - current_bpm) > 2)
    {
        current_bpm = test_bpm;
        update_bpm_timer(current_bpm);
    }
    vTaskDelay(pdMS_TO_TICKS(TEST_DELAY_MS));

    // Maximum
    ESP_LOGI(TAG, ">> Sending BPM 200 (maximum) — expect timer update");
    test_bpm = 200;
    if (abs(test_bpm - current_bpm) > 2)
    {
        current_bpm = test_bpm;
        update_bpm_timer(current_bpm);
    }
    vTaskDelay(pdMS_TO_TICKS(TEST_DELAY_MS));

    // Reset back to 90 for next test
    current_bpm = 90;
    update_bpm_timer(current_bpm);

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 4: No hands detected (Interrupt 4)
    // Sets audio_enabled = false, flushes DMA, then restores.
    // Expected: "Timer Fired" logs stop, then resume after re-enable.
    // ─────────────────────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "TEST 4: No hands detected (Interrupt 4)");
    ESP_LOGI(TAG, "----------------------------------------");

    ESP_LOGI(TAG, ">> NOHANDS received — disabling audio");
    audio_enabled = false;
    i2s_zero_dma_buffer(I2S_NUM);
    ESP_LOGI(TAG, "  audio_enabled = false — Timer Fired logs should stop");
    vTaskDelay(pdMS_TO_TICKS(4000)); // watch the silence

    ESP_LOGI(TAG, ">> Hand reappeared — re-enabling audio");
    audio_enabled = true;
    finger_down[0] = true;
    finger_down[2] = true;
    finger_down[4] = true;
    global_note_index = 0;
    ESP_LOGI(TAG, "  audio_enabled = true — Timer Fired logs should resume");
    vTaskDelay(pdMS_TO_TICKS(4000)); // watch it come back

    // ─────────────────────────────────────────────────────────────────────────
    // TEST 5: Rapid NOHANDS stress test
    // Alternates audio on/off quickly to check for state corruption.
    // Expected: clean transitions, no crashes, Timer Fired resumes each time.
    // ─────────────────────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "TEST 5: Rapid NOHANDS stress test");
    ESP_LOGI(TAG, "----------------------------------------");

    for (int i = 0; i < 5; i++)
    {
        ESP_LOGI(TAG, ">> Cycle %d/5 — NOHANDS", i + 1);
        audio_enabled = false;
        i2s_zero_dma_buffer(I2S_NUM);
        vTaskDelay(pdMS_TO_TICKS(500));

        ESP_LOGI(TAG, ">> Cycle %d/5 — Hand back", i + 1);
        audio_enabled = true;
        finger_down[0] = true;
        finger_down[2] = true;
        finger_down[4] = true;
        global_note_index = 0;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Done
    // ─────────────────────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ALL TESTS COMPLETE");
    ESP_LOGI(TAG, "  System left running with fingers 0,2,4");
    ESP_LOGI(TAG, "  active at %d BPM", current_bpm);
    ESP_LOGI(TAG, "========================================");

    vTaskDelete(NULL); // test task cleans itself up when done
}

void app_main(void)
{
    ESP_LOGI(TAG, "Invisible Piano starting...");

    gptimer_config_t config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
        .intr_priority = 3};

    gptimer_new_timer(&config, &beat_timer);

    gptimer_event_callbacks_t cbs = {
        .on_alarm = on_alarm_cb,
    };

    gptimer_register_event_callbacks(beat_timer, &cbs, NULL);

    gptimer_alarm_config_t alarm_config = {
        .alarm_count = (uint64_t)(60000000ULL / current_bpm), // period of timer
        .reload_count = 0,                                    // sets count back to 0 at reload
        .flags.auto_reload_on_alarm = true                    // enables auto reload
    };
    gptimer_set_alarm_action(beat_timer, &alarm_config);

    gptimer_enable(beat_timer);
    gptimer_start(beat_timer);

    uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, 0, 20, &uart_event_queue, 0);

    beat_semaphore = xSemaphoreCreateBinary();

    i2s_init();
    generate_note_buffers();

    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(uart_task, "uart_task", 4096, NULL, 5, NULL, 0);
    // xTaskCreatePinnedToCore(test_task, "test_task", 4096, NULL, 4, NULL, 0); // priority 4 — below uart(5) and audio(6)

    ESP_LOGI(TAG, "Tasks started");
}
