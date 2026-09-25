/**
 * @file test_signal_protocol.c
 * @brief 协议层宿主机单元测试
 */
#include <stdio.h>
#include <string.h>
#include "signal_protocol.h"
#include "app_config.h"

static int g_fail;

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      g_fail++;                                                \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__);            \
      printf(__VA_ARGS__);                                     \
      printf("\n");                                            \
    }                                                          \
  } while (0)

/**
 * DShot 校验参考实现(与被测代码相互独立地表述同一语义):
 * 12 位内容的 3 个半字节异或后取低 4 位,
 * 即 checksum = (frame ^ frame>>4 ^ frame>>8) & 0xF.
 * 这里按半字节逐个累加书写, 避免与被测代码共用同一表达式.
 */
static uint8_t ref_crc4(uint16_t frame12)
{
  uint16_t f      = (uint16_t)(frame12 & 0x0FFFU);
  uint8_t  nibble[3];
  uint8_t  acc = 0U;
  uint8_t  i;

  nibble[0] = (uint8_t)((f >> 8U) & 0x0FU);
  nibble[1] = (uint8_t)((f >> 4U) & 0x0FU);
  nibble[2] = (uint8_t)(f & 0x0FU);

  for (i = 0U; i < 3U; i++)
  {
    acc = (uint8_t)(acc ^ nibble[i]);
  }

  return (uint8_t)(acc & 0x0FU);
}

static void test_registry(void)
{
  uint8_t n = SignalProtocol_Count();

  CHECK(n == 7U, "期望注册 7 个协议, 实际 %u", n);

  /* 需求要求覆盖的协议族必须都在列表里 */
  CHECK(SignalProtocol_FindById(1U) != NULL, "PWM 缺失");
  CHECK(SignalProtocol_FindById(2U) != NULL, "OneShot125 缺失");
  CHECK(SignalProtocol_FindById(3U) != NULL, "OneShot42 缺失");
  CHECK(SignalProtocol_FindById(4U) != NULL, "MultiShot 缺失");
  CHECK(SignalProtocol_FindById(5U) != NULL, "DShot150 缺失");
  CHECK(SignalProtocol_FindById(6U) != NULL, "DShot300 缺失");
  CHECK(SignalProtocol_FindById(7U) != NULL, "DShot600 缺失");
  CHECK(SignalProtocol_FindById(99U) == NULL, "未知 ID 应返回 NULL");
  CHECK(SignalProtocol_At(7U) == NULL, "越界索引应返回 NULL");

  /* 名称匹配 DShot 速率 */
  CHECK(strcmp(SignalProtocol_FindById(5U)->name, "DShot150") == 0, "DShot150 名称不符");
  CHECK(strcmp(SignalProtocol_FindById(7U)->name, "DShot600") == 0, "DShot600 名称不符");
}

static void test_probe_all(void)
{
  uint8_t i;

  printf("[probe] 72MHz 下逐个探测\n");
  for (i = 0U; i < SignalProtocol_Count(); i++)
  {
    const SignalProtocol *p = SignalProtocol_At(i);
    SignalStatus          s = SignalProtocol_Probe(p, APP_TIMER_HZ);

    printf("  %-11s -> %s\n", p->name, SignalStatus_Text(s));
    CHECK(s == SIG_OK, "%s 在 72MHz 下应可用, 返回 %s", p->name, SignalStatus_Text(s));
  }
}

static void test_dshot_timings(void)
{
  const SignalProtocol *p150 = SignalProtocol_FindById(5U);
  const SignalProtocol *p300 = SignalProtocol_FindById(6U);
  const SignalProtocol *p600 = SignalProtocol_FindById(7U);

  printf("[dshot timings]\n");

  /* 72MHz / tick = 比特率 */
  CHECK(APP_TIMER_HZ / p150->tick_per_unit == 150000UL, "DShot150 位率错误: %lu",
        (unsigned long)(APP_TIMER_HZ / p150->tick_per_unit));
  CHECK(APP_TIMER_HZ / p300->tick_per_unit == 300000UL, "DShot300 位率错误");
  CHECK(APP_TIMER_HZ / p600->tick_per_unit == 600000UL, "DShot600 位率错误");

  /* 3/8 与 6/8 位周期必须为整数, 否则位宽失真 */
  CHECK((p150->tick_per_unit % 8U) == 0U, "DShot150 tick 不能被 8 整除");
  CHECK((p300->tick_per_unit % 8U) == 0U, "DShot300 tick 不能被 8 整除");
  CHECK((p600->tick_per_unit % 8U) == 0U, "DShot600 tick 不能被 8 整除");
}

static void test_pwm_plan(void)
{
  const SignalProtocol *p = SignalProtocol_FindById(1U);
  uint32_t              v[2];
  SignalPlan            plan;
  SignalStatus          s;

  printf("[pwm plan]\n");

  /* 1kHz, 50% */
  v[0] = 1000U;
  v[1] = 500U;
  s    = SignalProtocol_BuildPlan(p, v, APP_TIMER_HZ, 128U, &plan);
  CHECK(s == SIG_OK, "PWM 1kHz 解析失败: %s", SignalStatus_Text(s));
  /*
   * 1kHz 需要 72000 个计数, 超过 16 位上限 65536, 必须引入预分频.
   * 预分频 = ceil(72000/65536) = 2, 即 PSC=1; ARR = 72000/2 - 1 = 35999.
   */
  CHECK(plan.psc == 1U, "1kHz 期望 PSC=1 (需分频), 实际 %u", plan.psc);
  CHECK(plan.arr == 35999U, "1kHz 期望 ARR=35999, 实际 %u", plan.arr);
  CHECK(plan.ccr == 18000U, "50%% 期望 CCR=18000, 实际 %u", plan.ccr);
  CHECK(plan.period_us == 1000U, "期望周期 1000us, 实际 %lu", (unsigned long)plan.period_us);
  CHECK(plan.actual_hz == 1000U, "期望 1000Hz, 实际 %lu", (unsigned long)plan.actual_hz);

  /* 占空比 0% 与 100% 边界 */
  v[1] = 0U;
  CHECK(SignalProtocol_BuildPlan(p, v, APP_TIMER_HZ, 128U, &plan) == SIG_OK, "0%% 失败");
  CHECK(plan.ccr == 0U, "0%% 期望 CCR=0, 实际 %u", plan.ccr);

  v[1] = 1000U;
  CHECK(SignalProtocol_BuildPlan(p, v, APP_TIMER_HZ, 128U, &plan) == SIG_OK, "100%% 失败");
  CHECK(plan.ccr == plan.arr, "100%% 期望 CCR=ARR=%u, 实际 %u", plan.arr, plan.ccr);

  /* 参数越界必须被夹取, 而不是产生非法寄存器值 */
  v[0] = 1U;      /* 低于下限 50Hz */
  v[1] = 50000U;  /* 高于上限 1000 */
  CHECK(SignalProtocol_BuildPlan(p, v, APP_TIMER_HZ, 128U, &plan) == SIG_OK, "越界参数失败");
  CHECK(plan.param_values[0] == 50U, "频率应被夹到 50, 实际 %lu",
        (unsigned long)plan.param_values[0]);
  CHECK(plan.param_values[1] == 1000U, "占空比应被夹到 1000, 实际 %lu",
        (unsigned long)plan.param_values[1]);
  CHECK(plan.ccr <= plan.arr, "CCR 超过 ARR: %u > %u", plan.ccr, plan.arr);
}

static void test_oneshot_plan(void)
{
  const SignalProtocol *p = SignalProtocol_FindById(2U);
  uint32_t              v[2];
  SignalPlan            plan;
  SignalStatus          s;

  printf("[oneshot plan]\n");

  /* OneShot125: 帧周期 250us, 脉宽 125us -> 50% */
  v[0] = 125U;
  v[1] = 0U;
  s    = SignalProtocol_BuildPlan(p, v, APP_TIMER_HZ, 128U, &plan);
  CHECK(s == SIG_OK, "OneShot125 解析失败: %s", SignalStatus_Text(s));
  /* 250us * 72MHz = 18000 计数, ARR = 17999 */
  CHECK(plan.arr == 17999U, "期望 ARR=17999, 实际 %u", plan.arr);
  /* 125us * 72MHz = 9000 -> CCR = 9000 */
  CHECK(plan.ccr == 9000U, "期望 CCR=9000, 实际 %u", plan.ccr);
  CHECK(plan.period_us == 250U, "期望周期 250us, 实际 %lu", (unsigned long)plan.period_us);

  /* 脉宽上限必须严格小于周期, 否则没有低电平间隔 */
  v[0] = 250U;
  CHECK(SignalProtocol_BuildPlan(p, v, APP_TIMER_HZ, 128U, &plan) == SIG_OK, "上限脉宽失败");
  CHECK(plan.ccr < plan.arr, "脉宽占满整周期: CCR=%u ARR=%u", plan.ccr, plan.arr);

  /* 脉宽下限 1us 仍应产生非零 CCR */
  v[0] = 1U;
  CHECK(SignalProtocol_BuildPlan(p, v, APP_TIMER_HZ, 128U, &plan) == SIG_OK, "下限脉宽失败");
  CHECK(plan.ccr >= 1U, "1us 脉宽 CCR 应为非零, 实际 %u", plan.ccr);

  /* OneShot42 与 MultiShot 也要能解析 */
  {
    const SignalProtocol *q = SignalProtocol_FindById(3U);
    uint32_t              w[2] = {42U, 0U};
    CHECK(SignalProtocol_BuildPlan(q, w, APP_TIMER_HZ, 128U, &plan) == SIG_OK,
          "OneShot42 解析失败");
    /* 84us * 72MHz = 6048, ARR = 6047 */
    CHECK(plan.arr == 6047U, "OneShot42 期望 ARR=6047, 实际 %u", plan.arr);
    CHECK(plan.period_us == 84U, "OneShot42 期望周期 84us, 实际 %lu",
          (unsigned long)plan.period_us);
  }
  {
    const SignalProtocol *q = SignalProtocol_FindById(4U);
    uint32_t              w[2] = {12U, 0U};
    CHECK(SignalProtocol_BuildPlan(q, w, APP_TIMER_HZ, 128U, &plan) == SIG_OK,
          "MultiShot 解析失败");
    /* 30us * 72MHz = 2160, ARR = 2159 */
    CHECK(plan.arr == 2159U, "MultiShot 期望 ARR=2159, 实际 %u", plan.arr);
    CHECK(plan.period_us == 30U, "MultiShot 期望周期 30us, 实际 %lu",
          (unsigned long)plan.period_us);
  }
}

static void test_dshot_plan_and_frame(void)
{
  const SignalProtocol *p = SignalProtocol_FindById(7U); /* DShot600 */
  uint32_t              v[2];
  SignalPlan            plan;
  SignalStatus          s;

  printf("[dshot plan/frame]\n");

  v[0] = 500U;
  v[1] = 0U;
  s    = SignalProtocol_BuildPlan(p, v, APP_TIMER_HZ, 128U, &plan);
  CHECK(s == SIG_OK, "DShot600 解析失败: %s", SignalStatus_Text(s));

  /* 位周期 120 计数 -> ARR = 119; "0"=45, "1"=90 */
  CHECK(plan.arr == 119U, "DShot600 期望 ARR=119, 实际 %u", plan.arr);
  CHECK(plan.ccr_low == 45U, "DShot600 期望 CCR_low=45, 实际 %u", plan.ccr_low);
  CHECK(plan.ccr_high == 90U, "DShot600 期望 CCR_high=90, 实际 %u", plan.ccr_high);
  CHECK(plan.dma_len == 75U, "期望 DMA 长度 75 (looptime 125us), 实际 %u", plan.dma_len);
  CHECK(plan.frame_bits == 16U, "期望数据位 16, 实际 %u", plan.frame_bits);

  /* 比较值必须落在 ARR 之内, 且高低电平可区分 */
  CHECK(plan.ccr_low < plan.arr && plan.ccr_high < plan.arr + 1U,
        "DShot 比较值超出范围");

  /*
   * 帧重复率必须等于节流频率(125us -> 8kHz), 而不是"数据位后紧接重发".
   * 帧周期 = 位槽数 * 位周期 = 75 * (120/72MHz) = 125us.
   */
  CHECK(plan.period_us == 125U, "DShot600 帧周期应为 125us(8kHz), 实际 %lu",
        (unsigned long)plan.period_us);
  CHECK(plan.actual_hz == 8000U, "DShot600 帧率应为 8000Hz, 实际 %lu",
        (unsigned long)plan.actual_hz);

  /* 编码一帧: 检查位槽内容与 MSB 先发顺序 */
  {
    uint16_t buf[75];
    uint16_t i;

    s = SignalProtocol_EncodeDshotFrame(&plan, 500U, 0U, buf, 75U);
    CHECK(s == SIG_OK, "编码失败: %s", SignalStatus_Text(s));

    /* 停止位与空闲位必须是低电平(CCR=0) */
    CHECK(buf[16] == 0U, "停止位应为 0, 实际 %u", buf[16]);
    CHECK(buf[74] == 0U, "末位空闲应为 0, 实际 %u", buf[74]);

    /* 每个数据位只能是 ccr_low 或 ccr_high */
    for (i = 0U; i < plan.frame_bits; i++)
    {
      CHECK((buf[i] == plan.ccr_low) || (buf[i] == plan.ccr_high),
            "第 %u 位不是合法比较值: %u", i, buf[i]);
    }

    /* 与参考实现交叉验证 CRC */
    {
      uint16_t frame12 = SignalProtocol_DshotRawFrame(500U, 0U);
      uint8_t  mine    = SignalProtocol_DshotCrc(frame12);
      uint8_t  ref     = ref_crc4(frame12);

      printf("  frame12=0x%03X crc(impl)=0x%X crc(ref)=0x%X\n", frame12, mine, ref);
      CHECK(mine == ref, "CRC 与参考实现不一致: impl=0x%X ref=0x%X", mine, ref);

      /* CRC 应出现在最后 4 个数据位的比较值里 */
      {
        uint16_t full  = (uint16_t)((frame12 << 4) | mine);
        uint16_t index = 12U; /* 第 12..15 位是 CRC */

        for (i = index; i < plan.frame_bits; i++)
        {
          uint8_t want_bit = (uint8_t)((full >> (15U - i)) & 1U);
          uint16_t want    = (want_bit != 0U) ? plan.ccr_high : plan.ccr_low;

          CHECK(buf[i] == want, "第 %u 位 CRC 编码错误: 实际 %u 期望 %u", i, buf[i], want);
        }
      }
    }
  }
}

static void test_dshot_crc_vectors(void)
{
  /* 已知边界帧: 停机(0) 与 最大油门(2047), 遥测位开/关 */
  struct { uint16_t throttle; uint8_t telem; } cases[] = {
    {0U, 0U}, {1U, 0U}, {48U, 0U}, {500U, 0U},
    {1000U, 0U}, {2047U, 0U}, {2047U, 1U}, {0U, 1U},
  };
  uint8_t i;

  printf("[dshot crc vectors]\n");
  for (i = 0U; i < (uint8_t)(sizeof(cases) / sizeof(cases[0])); i++)
  {
    uint16_t f   = SignalProtocol_DshotRawFrame(cases[i].throttle, cases[i].telem);
    uint8_t  imp = SignalProtocol_DshotCrc(f);
    uint8_t  ref = ref_crc4(f);

    printf("  thr=%-5u t=%u frame=0x%03X crc=0x%X ref=0x%X\n",
           cases[i].throttle, cases[i].telem, f, imp, ref);
    CHECK(imp == ref, "CRC 不一致 (thr=%u t=%u): 0x%X vs 0x%X",
          cases[i].throttle, cases[i].telem, imp, ref);
    CHECK(imp <= 0x0FU, "CRC 超出 4 位: 0x%X", imp);
  }

  /* 油门夹取: 越界输入不得产生非法握手值 */
  CHECK(SignalProtocol_DshotRawFrame(3000U, 0U) == SignalProtocol_DshotRawFrame(2047U, 0U),
        "油门超上限应夹到 2047");
  CHECK(SignalProtocol_DshotRawFrame(0U, 0U) == 0U, "油门 0 应保持 0(停机)");
}

static void test_failure_paths(void)
{
  const SignalProtocol *p = SignalProtocol_FindById(7U);
  uint32_t              v[2] = {500U, 0U};
  SignalPlan            plan;

  printf("[failure paths]\n");

  /* 帧缓冲太小必须报 FRAMELONG, 而不是越界写 */
  {
    uint16_t      small[20];
    SignalStatus  s = SignalProtocol_BuildPlan(p, v, APP_TIMER_HZ, 20U, &plan);
    CHECK(s == SIG_ERR_FRAME_TOO_LONG, "缓冲不足应返回 FRAMELONG, 实际 %s",
          SignalStatus_Text(s));

    /* 直接调用编码器也要拦截 */
    if (SignalProtocol_BuildPlan(p, v, APP_TIMER_HZ, 128U, &plan) == SIG_OK)
    {
      s = SignalProtocol_EncodeDshotFrame(&plan, 500U, 0U, small, 20U);
      CHECK(s == SIG_ERR_FRAME_TOO_LONG, "编码器未拦截小缓冲, 实际 %s",
            SignalStatus_Text(s));
    }
  }

  /* 定时器时钟过低: DShot 位周期超过 16 位 ARR 或无法被 8 整除 */
  CHECK(SignalProtocol_Probe(p, 1000000UL) != SIG_OK, "1MHz 定时器不应支持 DShot600");

  /* DShot1200 情形: tick=60 不能被 8 整除, 必须被拒绝 */
  {
    SignalProtocol bad = *p;

    bad.tick_per_unit = 60U; /* 1e9/(1.2e6) * 72e6 / 1e9 = 60 */
    CHECK(SignalProtocol_Probe(&bad, APP_TIMER_HZ) == SIG_ERR_TICK_TOO_FAST,
          "tick=60 应被 TICKFAST 拒绝");
  }

  /* 空指针保护 */
  CHECK(SignalProtocol_BuildPlan(NULL, v, APP_TIMER_HZ, 128U, &plan) == SIG_ERR_NULL_HANDLE,
        "空协议指针未拦截");
  CHECK(SignalProtocol_BuildPlan(p, NULL, APP_TIMER_HZ, 128U, &plan) == SIG_ERR_NULL_HANDLE,
        "空参数指针未拦截");
  CHECK(SignalProtocol_BuildPlan(p, v, APP_TIMER_HZ, 128U, NULL) == SIG_ERR_NULL_HANDLE,
        "空输出指针未拦截");
  CHECK(SignalProtocol_Probe(NULL, APP_TIMER_HZ) == SIG_ERR_NULL_HANDLE,
        "空协议探测未拦截");

  /* 非 DShot 协议不能走 DShot 编码器 */
  {
    const SignalProtocol *pwm = SignalProtocol_FindById(1U);
    uint32_t              w[2] = {1000U, 500U};
    uint16_t              buf[18];

    if (SignalProtocol_BuildPlan(pwm, w, APP_TIMER_HZ, 128U, &plan) == SIG_OK)
    {
      plan.proto = pwm;
      CHECK(SignalProtocol_EncodeDshotFrame(&plan, 500U, 0U, buf, 75U) == SIG_ERR_UNSUPPORTED,
            "PWM 协议不应接受 DShot 编码");
    }
  }
}

static void test_param_stepping(void)
{
  const SignalProtocol *p = SignalProtocol_FindById(1U);

  printf("[param stepping]\n");

  /* 占空比限幅: 0.1% 步进, 范围 0..1000 */
  CHECK(SignalProtocol_StepParam(p, 1U, 1000U, 1) == 1000U, "占空比上限应夹住");
  CHECK(SignalProtocol_StepParam(p, 1U, 0U, -1) == 0U, "占空比下限应夹住");
  CHECK(SignalProtocol_StepParam(p, 1U, 500U, 1) == 510U, "占空比步进应为 10");
  CHECK(SignalProtocol_StepParam(p, 1U, 500U, -1) == 490U, "占空比反向步进应为 10");

  /* 频率使用对数步进: 低端小步, 高端大步 */
  {
    uint32_t low  = SignalProtocol_StepParam(p, 0U, 100U, 1);
    uint32_t high = SignalProtocol_StepParam(p, 0U, 10000U, 1);

    printf("  freq step @100 -> %lu, @10000 -> %lu\n",
           (unsigned long)low, (unsigned long)high);
    CHECK((low - 100U) < (high - 10000U) || high == 20000U,
          "频率步进未体现对数特性");
  }

  /* 无效参数槽 */
  CHECK(SignalProtocol_ParamAt(p, 5U) == NULL, "越界参数槽应为 NULL");
  CHECK(SignalProtocol_ClampParam(p, 5U, 100U) == 0U, "无效槽夹取应返回 0");
  CHECK(SignalProtocol_DefaultParam(p, 0U) == 1000U, "PWM 默认频率应为 1000Hz");
}

int main(void)
{
  printf("=== signal_protocol 单元测试 ===\n");

  test_registry();
  test_probe_all();
  test_dshot_timings();
  test_pwm_plan();
  test_oneshot_plan();
  test_dshot_plan_and_frame();
  test_dshot_crc_vectors();
  test_failure_paths();
  test_param_stepping();

  printf("=== %s (失败 %d 项) ===\n", g_fail == 0 ? "全部通过" : "存在失败", g_fail);
  return (g_fail == 0) ? 0 : 1;
}
