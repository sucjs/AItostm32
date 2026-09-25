/**
 * @file test_knob.c
 * @brief 电位器滤波与归一化的宿主机单元测试
 * @note  knob.c 不含任何 STM32 头文件, 原始样本通过 Knob_AdcSample() 钩子注入.
 *        这里提供该钩子的宿主机实现, 从而用确定的样本序列验证滤波与归一化.
 */
#include <stdio.h>
#include "knob.h"
#include "app_config.h"

static int g_fail;

#define CHECK(cond, ...)                            \
  do {                                              \
    if (!(cond)) {                                  \
      g_fail++;                                     \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                          \
      printf("\n");                                 \
    }                                               \
  } while (0)

/* ========================== 采样钩子的宿主机实现 ========================== */

static uint16_t g_adc;

uint16_t Knob_AdcSample(void)
{
  return g_adc;
}

/* ========================== 工具 ========================== */

/** 反复喂同一个原始值, 直到两个滤波窗口都被它填满并稳定 */
static void settle(uint16_t raw)
{
  int i;

  for (i = 0; i < (int)(KNOB_MOVAVG_WINDOW + KNOB_MEDIAN_WINDOW + 4U); i++)
  {
    Knob_PushSample(raw);
  }
}

/* ========================== 测试项 ========================== */

/** 中值滤波必须整体剔除单个尖峰 */
static void test_median_rejects_spike(void)
{
  uint16_t before;

  printf("[median] 单个尖峰应被完全剔除\n");
  Knob_Init();
  settle(2000U);

  before = Knob_GetFiltered();
  CHECK(before == 2000U, "稳定后滤波值应为 2000, 实际 %u", before);

  /* 注入一个孤立大尖峰 */
  Knob_PushSample(4000U);

  CHECK(Knob_GetRaw() == 4000U, "原始值应更新为 4000, 实际 %u", Knob_GetRaw());
  CHECK(Knob_GetFiltered() == before,
        "中值应丢掉尖峰, 滤波值不应变(期望 %u, 实际 %u)", before, Knob_GetFiltered());

  /* 再喂正常值, 应完全不受影响 */
  Knob_PushSample(2000U);
  CHECK(Knob_GetFiltered() == 2000U, "尖峰后应回到 2000, 实际 %u", Knob_GetFiltered());
}

/** 滑动平均必须对真实变化给出渐进响应(而不是一步跳到位) */
static void test_movavg_smooths(void)
{
  uint16_t after_one;
  uint16_t after_two;
  uint16_t after_many;

  printf("[movavg] 阶跃变化应被平滑\n");
  Knob_Init();
  settle(2000U);

  /* 目标值 3000: 中值需 2 帧才越过(窗口 3), 之后滑动平均逐帧逼近 */
  Knob_PushSample(3000U);
  after_one = Knob_GetFiltered();
  CHECK(after_one == 2000U, "第 1 帧中值仍为 2000, 滤波值不应变, 实际 %u", after_one);

  Knob_PushSample(3000U);
  after_two = Knob_GetFiltered();
  CHECK(after_two > 2000U, "第 2 帧应开始向 3000 移动, 实际 %u", after_two);
  CHECK(after_two < 3000U, "第 2 帧不应一步到位 3000, 实际 %u", after_two);

  settle(3000U);
  after_many = Knob_GetFiltered();
  CHECK(after_many == 3000U, "充分稳定后应为 3000, 实际 %u", after_many);
}

/** 归一化端点: 有效区下界 -> 0, 上界 -> KNOB_LEVEL_MAX */
static void test_normalize_endpoints(void)
{
  uint16_t level;

  printf("[normalize] 端点与中点\n");
  Knob_Init();

  settle(KNOB_RAW_MIN_VALID);
  level = Knob_GetLevel();
  CHECK(level == 0U, "原始值=%u 时档位应为 0, 实际 %u", (unsigned)KNOB_RAW_MIN_VALID, level);

  Knob_Init();
  settle(KNOB_RAW_MAX_VALID);
  level = Knob_GetLevel();
  CHECK(level == KNOB_LEVEL_MAX, "原始值=%u 时档位应为 %u, 实际 %u",
        (unsigned)KNOB_RAW_MAX_VALID, (unsigned)KNOB_LEVEL_MAX, level);

  /* 中点: 20 + 4055/2 = 2047, 归一化应约为半量程 */
  Knob_Init();
  settle((uint16_t)(KNOB_RAW_MIN_VALID + (KNOB_RAW_MAX_VALID - KNOB_RAW_MIN_VALID) / 2U));
  level = Knob_GetLevel();
  CHECK((level >= 490U) && (level <= 510U), "中点档位应接近 500, 实际 %u", level);
}

/** 超出有效区的原始值必须夹到两端, 不得越界或回绕 */
static void test_out_of_range_clamps(void)
{
  uint16_t level;

  printf("[clamp] 越界原始值应夹到两端\n");
  Knob_Init();

  /* 远低于有效下界 */
  settle(0U);
  level = Knob_GetLevel();
  CHECK(level == 0U, "原始值 0 应夹到档位 0, 实际 %u", level);

  /* 远高于有效上界(ADC 满量程) */
  Knob_Init();
  settle(KNOB_ADC_MAX);
  level = Knob_GetLevel();
  CHECK(level == KNOB_LEVEL_MAX, "原始值 %u 应夹到档位 %u, 实际 %u",
        (unsigned)KNOB_ADC_MAX, (unsigned)KNOB_LEVEL_MAX, level);

  /* 档位任何情况下都不得超出 0..KNOB_LEVEL_MAX */
  CHECK(level <= KNOB_LEVEL_MAX, "档位越界: %u", level);
}

/** 变化标志: 首次上报置位, 死区内抖动不置位, 超过死区再次置位 */
static void test_changed_deadband(void)
{
  uint16_t stable_raw = (uint16_t)(KNOB_RAW_MIN_VALID +
                                   (KNOB_RAW_MAX_VALID - KNOB_RAW_MIN_VALID) / 2U);
  uint16_t small_raw;
  uint16_t big_raw;

  printf("[changed] 死区行为\n");
  Knob_Init();
  settle(stable_raw);

  /* 首次读取应置位(基线从 0 变为当前档位) */
  CHECK(Knob_Changed() != 0U, "首次读取变化标志应置位");
  CHECK(Knob_Changed() == 0U, "再次读取应清除, 返回 0");

  /* 原值不动: 不得置位 */
  settle(stable_raw);
  CHECK(Knob_Changed() == 0U, "档位未变时不应置位");

  /*
   * 小位移: 目标档位与当前基线相差约 3(< KNOB_LEVEL_DEADBAND=4), 不应置位.
   * 档位 502 对应滤波值 2056 附近.
   */
  small_raw = 2056U;
  settle(small_raw);
  CHECK(Knob_GetLevel() >= 499U && Knob_GetLevel() <= 502U,
        "小位移后档位应在预期范围内, 实际 %u", Knob_GetLevel());
  CHECK(Knob_Changed() == 0U, "死区内的位移不应置位(当前档位 %u)", Knob_GetLevel());

  /*
   * 大位移: 目标档位约 510(与基线差 > 4), 必须置位.
   * 档位 510 对应滤波值 2089 附近.
   */
  big_raw = 2089U;
  settle(big_raw);
  CHECK(Knob_Changed() != 0U, "超过死区的位移应置位(当前档位 %u)", Knob_GetLevel());
}

/** Knob_Update 必须走 Knob_AdcSample 钩子取样本 */
static void test_update_uses_hook(void)
{
  printf("[update] Knob_Update 应读取采样钩子\n");
  Knob_Init();

  g_adc = 1234U;
  Knob_Update();
  CHECK(Knob_GetRaw() == 1234U, "应取到钩子返回值 1234, 实际 %u", Knob_GetRaw());

  g_adc = 3000U;
  Knob_Update();
  CHECK(Knob_GetRaw() == 3000U, "应取到钩子返回值 3000, 实际 %u", Knob_GetRaw());
}

int main(void)
{
  printf("=== knob 单元测试 ===\n");

  test_median_rejects_spike();
  test_movavg_smooths();
  test_normalize_endpoints();
  test_out_of_range_clamps();
  test_changed_deadband();
  test_update_uses_hook();

  printf("=== %s (失败 %d 项) ===\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
  return (g_fail == 0) ? 0 : 1;
}
