# Fix Plans

## FIX-001: 修正 uartIrqHandler 中 ORE 清除分支的门控 (date: 2026-06-10)

### 修复方案

1. 还原被错误怀疑并已暂存 revert 的良性提交 `1ae95c1bb`(MSP CLI 竞态修复):`git revert --abort`。(已完成)
2. 修改 `src/platform/STM32/serial_uart_hal.c` `uartIrqHandler()` 的 ORE 分支门控,使其匹配硬件真实的中断触发条件(RXNEIE 或 EIE 任一使能即可触发 ORE 中断):

```c
/* UART Over-Run interrupt occurred */
if (((cr1 & USART_CR1_RXNEIE) || (cr3 & USART_CR3_EIE)) &&
    (__HAL_UART_GET_IT(huart, UART_IT_ORE) != RESET)) {
    __HAL_UART_CLEAR_IT(huart, UART_CLEAR_OREF);
}
```

(最终宏名/是否需含 RXFTIE 以 Oracle 对 H7/F7/G4 CMSIS 头文件核查结果为准。)

3. 检查同一 handler 内其余分支(PE/FE/NE/TC/TXE/IDLE)是否存在同类"门控使能源与硬件触发源不一致"的问题,如有一并修复。

### 验证方法

1. `make KineticoH7 -j128` 编译干净。
2. 真机:接着接收机/眼镜(保证 UART RX 流量)连续保存参数多次,不再卡死、LED 正常、保存后正常重启。
3. 旁证(可选):坏固件拔掉所有 UART 外设保存不卡 → 证明触发条件是 RX 流量。
