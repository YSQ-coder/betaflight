# Issues

## ISS-001: 参数保存时飞控卡死(UART ORE 中断风暴死锁) (严重)

| 项目 | 内容 |
|------|------|
| 状态 | 已解决 |
| resolved | 2026-06-11 |
| 发现日期 | 2026-06-10 |
| 相关文件 | `src/platform/STM32/serial_uart_hal.c` |
| 引入提交 | `db7df6e48` ([backport] fix(uart): guard half-duplex pinless UART modes (#15218) (#15258),upstream 2025.12.3 引入) |
| 测试硬件 | KineticoH7 (STM32H743) |
| 上游对应 | betaflight/betaflight#15317、#15306(均 OPEN,F7 目标同症状,已用 gh 确认真实存在) |

### 问题描述

- 保存参数(Configurator Save / CLI save / CMS 菜单)时飞控卡死:不重启、Configurator 界面卡住、状态 LED 完全停止闪烁,需断电恢复。
- 断电重启后飞控正常、历史参数完好,仅最后一次修改未写入 → CPU 死在 flash 配置扇区擦除之前。
- 复现与"动了哪个参数"无关,真实变量是保存瞬间是否存在 UART RX 流量(CRSF 420k / GPS / MSP DisplayPort)。
- 版本定位:5月7日构建(分支 `64749122b`,≈2025.12.2)正常;当前 HEAD(≈2025.12.5-alpha)必现。回归窗口内与 H7 运行时相关的 upstream 改动仅 4 个提交,其中唯一触碰中断路径的是 `db7df6e48`。
- 上游 #15317(Velox F7 SE)、#15306(SKYSTARSF7HDPRO)报告"12.1/12.2 正常、12.4 保存时锁死",与本问题同源(F7/H7 共用 `serial_uart_hal.c`)。

### 原因分析

`db7df6e48` 重写了 `uartIrqHandler()`,把每个 `__HAL_UART_GET_IT` 分支用入口缓存的 CR1/CR3 中断使能位做了门控。其中 ORE(接收溢出)分支被门控为:

```c
if ((cr3 & USART_CR3_EIE) && (__HAL_UART_GET_IT(huart, UART_IT_ORE) != RESET)) {
    __HAL_UART_CLEAR_IT(huart, UART_CLEAR_OREF);
}
```

但 Betaflight 自身的 RXNE 分支**每收到一个字节就执行 `CLEAR_BIT(CR3, USART_CR3_EIE)`**(历史遗留行为),因此活跃接收的 IRQ 驱动 UART 上 `CR3.EIE` 恒为 0,ORE 永远不会被清除。

而按 STM32 H7/F7/G4 参考手册语义,**`ORE` 在 `CR1.RXNEIE=1` 时同样触发 USART 中断**(不依赖 `CR3.EIE`)。于是:

1. 保存参数 → `writeEEPROM()` 擦写内部 flash,CPU/中断被长时间阻塞;
2. CRSF/GPS/OSD 串口字节无人接收 → 硬件置位 ORE;
3. 中断恢复后:RXNE 已被读空 → RXNE 分支跳过;ORE 置位但 `cr3.EIE==0` → 清除分支被跳过;
4. 退出 ISR 后 ORE 仍置位且 RXNEIE=1 → 中断立刻重入 → **无限中断重入(livelock)**;
5. 线程态永久饿死:LED 不闪、MSP 无响应、flash 写入永远不会执行。

旧代码(5月7日好固件)对 ORE 是**无条件清除**,不存在此问题。

### 影响范围

所有使用 `src/platform/STM32/serial_uart_hal.c` 的 MCU 家族(F7 / H7 / G4 等 HAL 驱动平台)上运行 2025.12.3 / 2025.12.4 固件且任一 IRQ 驱动 UART 有接收流量的 target。F4(stdperiph 驱动)不受影响。
