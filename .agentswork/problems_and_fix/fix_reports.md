# Fix Reports

## 2026-06-10 FIX-001 执行记录

- 修复范围: `src/platform/STM32/serial_uart_hal.c`(ORE 门控);`src/main/msp/msp_serial.c`(撤销错误 revert,恢复 HEAD)
- 关键修复点: ORE 清除分支门控加入 `CR1.RXNEIE`;保留 `1ae95c1bb` 良性 MSP 修复

### explore (round 1)
- 结果: 根因定位为 `db7df6e48` 的 ORE 门控缺陷
- 详情:
  - 回归窗口锁定: 好=分支 `64749122b`(5月7日构建), 坏=HEAD `afcd18e7b`
  - H7 运行时相关 delta 仅 4 个 upstream 提交,唯一触碰中断路径的是 `db7df6e48`
  - 证据链: LED 全停(线程态饿死)、参数未丢(擦除前死亡)、多保存路径触发(公共 flash 阻塞窗口)、与具体参数无关(实为 RX 流量依赖)
  - librarian 确认上游同症状报告 #15317/#15306(gh 核实真实存在, OPEN)
  - Oracle 初判: ORE livelock,置信度高;二次确认(含 RDR/RXFRQ 是否旁路清除 ORE 的反证检查)进行中

### execute (round 1)
- 结果: `git revert --abort` 完成,`msp_serial.c` 恢复为 HEAD(保留 1ae95c1bb)
- 详情:
  - 工作区剩余改动仅 clangd 缓存与 `src/config` 子模块的 `ICM426XX_CLOCK`(用户既有改动,保留)


### review (round 1)
- 结果: Oracle 二次确认缺陷成立(CONFIRMED),补丁形态确定
- 详情:
  - 反证检查通过: RDR 读取与 RQR.RXFRQ 均不清 ORE,只有 ICR.ORECF 能清(stm32h7xx_hal_uart.h:766/509)→ 死锁成立
  - ST HAL 自身对 ORE 的使能判定为 RXNEIE || EIE(F7: stm32f7xx_hal_uart.c:2157, H7: stm32h7xx_hal_uart.c:2234, G4: stm32g4xx_hal_uart.c:2214)
  - `serial_uart_hal.c` 编译范围: F7 / G4 / H7 / H5(F4 用 stdperiph 不受影响)
  - `USART_CR1_RXNEIE` 在四个家族 CMSIS 头中均可用(H7 为 RXNEIE_RXFNEIE 别名)
  - 其余分支(PE/FE/NE/TC/TXE/IDLE)门控与硬件触发源一致,无同类残留缺陷;RXFTIE/FIFO 模式未启用

### execute (round 2)
- 结果: 补丁已应用并编译通过
- 详情:
  - `serial_uart_hal.c` ORE 分支门控改为 `(cr1 & USART_CR1_RXNEIE) || (cr3 & USART_CR3_EIE)`,附注释说明机理
  - `make KineticoH7 -j128` 成功 → `obj/betaflight_2025.12.5-alpha_STM32H743_KineticoH7.hex`
  - 待真机验证(接收机/眼镜在线连续保存)后将 ISS-001 置为 已解决

### conclusion
- 用户决策: 真机验证通过("问题已经解决"),要求 commit + PR
- 结论: ORE 门控修复有效,保存不再卡死。修复已提交本地分支,并向 upstream betaflight/betaflight 的 2025.12-maintenance 分支提交 PR(master 不受影响,其 serial_uart_ll.c 无条件清 ORE)。ISS-001 关闭