#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "fpioa.h"
#include "plic.h"
#include "sysctl.h"
#include "uarths.h"
#include "utils.h"
#include "kpu.h"
#include "w25qxx.h"
#include "iomem.h"
#include "lpbox.h"
#include "nt35310.h"
#include "lcd.h"

#define KPU_DEBUG 0

#if KPU_DEBUG
#define PRINTF_KPU_OUTPUT(output, size)                          \
    {                                                            \
        printf("%s addr is %ld\n", #output, (uint64_t)(output)); \
        printf("[");                                             \
        for (size_t i=0; i < (size); i++) {                      \
            if (i%5 == 0) {                                      \
                printf("\n");                                    \
            }                                                    \
            printf("%f, ", *((output)+i));                       \
        }                                                        \
        printf("]\n");                                           \ 
    }
#else  // KPU_DEBUG
#define PRINTF_KPU_OUTPUT(output, size) {printf("%s addr is %ld\n", #output, (uint64_t)(output));}
#endif // KPU_DEBUG

const size_t layer0_w = 19, layer0_h = 14;
const size_t layer1_w = 9 , layer1_h = 6;

const size_t rf_stride0 = 16 , rf_stride1 = 32;
const size_t rf_start0  = 15 , rf_start1  = 31;
const size_t rf_size0   = 128, rf_size1   = 256;

#define PLL0_OUTPUT_FREQ 800000000UL
#define PLL1_OUTPUT_FREQ 400000000UL

extern const unsigned char gImage_image[] __attribute__((aligned(128)));

static uint16_t lcd_gram[320 * 240] __attribute__((aligned(32)));

#define LPBOX_KMODEL_SIZE (556864)
uint8_t* lpbox_model_data;

kpu_model_context_t task;

uint8_t lpimage[24 * 94 * 3] __attribute__((aligned(32)));

volatile uint8_t g_ai_done_flag;

static void ai_done(void* arg)
{
    g_ai_done_flag = 1;
}

static void io_mux_init(void)
{
    /* Init DVP IO map and function settings */
    fpioa_set_function(42, FUNC_CMOS_RST);
    fpioa_set_function(44, FUNC_CMOS_PWDN);
    fpioa_set_function(46, FUNC_CMOS_XCLK);
    fpioa_set_function(43, FUNC_CMOS_VSYNC);
    fpioa_set_function(45, FUNC_CMOS_HREF);
    fpioa_set_function(47, FUNC_CMOS_PCLK);
    fpioa_set_function(41, FUNC_SCCB_SCLK);
    fpioa_set_function(40, FUNC_SCCB_SDA);

    /* Init SPI IO map and function settings */
    fpioa_set_function(38, FUNC_GPIOHS0 + DCX_GPIONUM);
    fpioa_set_function(36, FUNC_SPI0_SS3);
    fpioa_set_function(39, FUNC_SPI0_SCLK);
    fpioa_set_function(37, FUNC_GPIOHS0 + RST_GPIONUM);

    sysctl_set_spi0_dvp_data(1);
}

static void io_set_power(void)
{
    /* Set dvp and spi pin to 1.8V */
    sysctl_set_power_mode(SYSCTL_POWER_BANK6, SYSCTL_POWER_V18);
    sysctl_set_power_mode(SYSCTL_POWER_BANK7, SYSCTL_POWER_V18);
}

void rgb888_to_lcd(uint8_t* src, uint16_t* dest, size_t width, size_t height)
{
    size_t i, chn_size = width * height;
    for (size_t i = 0; i < width * height; i++) {
        uint8_t r = src[i];
        uint8_t g = src[chn_size + i];
        uint8_t b = src[chn_size * 2 + i];

        uint16_t rgb = ((r & 0b11111000) << 8) | ((g & 0b11111100) << 3) | (b >> 3);
        size_t d_i = i % 2 ? (i - 1) : (i + 1);
        dest[d_i] = rgb;
    }
}

int main()
{
    // 配置时钟
    long pll0 = sysctl_pll_set_freq(SYSCTL_PLL0, PLL0_OUTPUT_FREQ);
    long pll1 = sysctl_pll_set_freq(SYSCTL_PLL1, PLL1_OUTPUT_FREQ);
    printf("\nPLL0 = %ld PLL1 = %ld\n", pll0, pll1);
    uarths_init();

    io_mux_init();
    io_set_power();

    plic_init();

    // LCD init
    printf("LCD init\n");
    lcd_init();
    lcd_set_direction(DIR_YX_RLDU);
    printf("LCD end\n");

    // 加载模型
    printf("flash init\n");
    w25qxx_init(3, 0);
    w25qxx_enable_quad_mode();
    lpbox_model_data = (uint8_t*)malloc(LPBOX_KMODEL_SIZE);
    w25qxx_read_data(0xA00000, lpbox_model_data, LPBOX_KMODEL_SIZE, W25QXX_QUAD_FAST);

    sysctl_enable_irq();

    // 加载模型
    printf("\nmodel init start\n");
    if (kpu_load_kmodel(&task, lpbox_model_data) != 0) {
        printf("model init error\n");
        while (1)
            ;
    }
    printf("\nmodel init OK\n");

    // 运行模型
    g_ai_done_flag = 0;
    kpu_run_kmodel(&task, gImage_image, DMAC_CHANNEL5, ai_done, NULL);
    printf("\nmodel run start\n");
    while (!g_ai_done_flag)
        ;
    printf("\nmodel run OK\n");

/*********************************************************************/
    printf("lpbox init\n");
    lpbox_t lpbox;
    if (lpbox_new(&lpbox, 2) != 0) {
        printf("lpbox new error/n");
        while (1)
            ;
    }
    (lpbox.kpu_output)[0].w         = layer0_w;
    (lpbox.kpu_output)[0].h         = layer0_h;
    (lpbox.kpu_output)[0].rf_size   = rf_size0;
    (lpbox.kpu_output)[0].rf_start  = rf_start0;
    (lpbox.kpu_output)[0].rf_stride = rf_stride0;

    (lpbox.kpu_output)[1].w         = layer1_w;
    (lpbox.kpu_output)[1].h         = layer1_h;
    (lpbox.kpu_output)[1].rf_size   = rf_size1;
    (lpbox.kpu_output)[1].rf_start  = rf_start1;
    (lpbox.kpu_output)[1].rf_stride = rf_stride1;
    printf("lpbox init end");
/********************************************************************/

    // 提取模型推理结果
    float *score_layer0, *score_layer1, *bbox_layer0, *bbox_layer1;

    size_t score_layer0_size;
    kpu_get_output(&task, 0, &score_layer0, &score_layer0_size);
    score_layer0_size /= 4;
    printf("\nscore_layer0_size: %ld\n", score_layer0_size);
    (lpbox.kpu_output)[0].score_layer = score_layer0;
    PRINTF_KPU_OUTPUT((score_layer0), (score_layer0_size));

    size_t bbox_layer0_size;
    kpu_get_output(&task, 1, &bbox_layer0, &bbox_layer0_size);
    bbox_layer0_size /= 4;
    printf("bbox_layer0_size: %ld\n", bbox_layer0_size);
    (lpbox.kpu_output)[0].bbox_layer = bbox_layer0;
    PRINTF_KPU_OUTPUT((bbox_layer0), (bbox_layer0_size));

    size_t bbox_layer1_size;
    kpu_get_output(&task, 2, &bbox_layer1, &bbox_layer1_size);
    bbox_layer1_size /= 4;
    printf("bbox_layer1_size: %ld\n", bbox_layer1_size);
    (lpbox.kpu_output)[1].bbox_layer = bbox_layer1;
    PRINTF_KPU_OUTPUT((bbox_layer1), (bbox_layer1_size));

    size_t score_layer1_size;
    kpu_get_output(&task, 3, &score_layer1, &score_layer1_size);
    score_layer1_size /= 4;
    printf("score_layer1_size: %ld\n", score_layer1_size);
    (lpbox.kpu_output)[1].score_layer = score_layer1;
    PRINTF_KPU_OUTPUT((score_layer1), (score_layer1_size));

    // 提取预测框
    
    printf("\nLPbox run start\n");
    get_lpbox(&lpbox, 0.8, 0.2);
    printf("\nLPbox run OK\n");

    printf("bbox num：%d\n", lpbox.bboxes->num);

    // 显示图片
    rgb888_to_lcd(gImage_image, lcd_gram, 320, 240);
    lcd_draw_picture(0, 0, 320, 240, lcd_gram);
    
    for (bbox_t* bbox = lpbox.bboxes->box; bbox != NULL; bbox = bbox->next) {
        printf("x1: %f, y1: %f, x2: %f, y2: %f, score: %f\n", bbox->x1, bbox->y1, bbox->x2, bbox->y2, bbox->score);
        lcd_draw_rectangle(bbox->x1, bbox->y1, bbox->x2, bbox->y2, 2, GREEN);
    }

    printf("\nend\n");

    while (1)
        ;
}