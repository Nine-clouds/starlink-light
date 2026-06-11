/*****************************************************************
 * 协议：自定义帧头帧尾串口协议（从机，支持 Toggle）
 * MCU : STC8H1K08, 11.0592MHz, 9600bps
 * UART: P3.6(RxD), P3.7(TxD)
 * 指令：0x01-开灯, 0x02-关灯, 0x03-查询, 0x04-切换状态
 * 其他：广播可执行（乐观更新，不回复）、看门狗、回溯、上电恢复状态
 * 优化：延迟写入 + wear leveling（减少EEPROM磨损）
 * 修改：高电平开灯，低电平关灯
 ******************************************************************/
#include "STC8H.h"
#include <intrins.h>

typedef unsigned char uint8_t;
typedef unsigned int  uint16_t;

/*********************** 用户配置 ***********************/
#define DEVICE_ADDR     0x06
#define FOSC            11059200UL
#define BAUD            9600
#define BROADCAST_REPLY 0       // 广播查询时不回复（乐观更新）

#define DEFAULT_LIGHT_STATE 0   // 默认关灯（低电平）

// 延迟写入时间（毫秒）
#define SAVE_DELAY_MS       2000
/*******************************************************/

sbit STATE_LED = P1^0;            // 高电平开灯，低电平关灯

/* 帧字节常量 */
#define FRAME_HEAD1  0x10
#define FRAME_HEAD2  0x18
#define FRAME_TAIL1  0x18
#define FRAME_TAIL2  0x10

#define CMD_ON       0x01
#define CMD_OFF      0x02
#define CMD_QUERY    0x03
#define CMD_TOGGLE   0x04

/* 接收状态机 */
enum {
    WAIT_HEAD1 = 0,
    WAIT_HEAD2,
    WAIT_ADDR,
    WAIT_CMD,
    WAIT_TAIL1,
    WAIT_TAIL2
};

/*********************** 全局变量 ***********************/
uint8_t rx_state = WAIT_HEAD1;
volatile uint8_t rx_buf[2];     // ISR写，主循环读
volatile uint8_t recv_ok = 0;   // ISR写，主循环读，必须volatile
uint8_t g_light_on = 0;

// 帧超时复位（收到首字节后50ms内未完成则丢弃）
#define RX_TIMEOUT_MS   50
volatile uint8_t rx_timeout = 0;

// 延迟写入相关
volatile uint8_t need_save = 0;      // 有待保存的状态
volatile uint8_t pending_state = 0;  // 待保存的状态值
volatile uint16_t save_timer = 0;    // 剩余等待时间(ms)
volatile uint8_t do_save = 0;        // 主循环中执行保存的标志

/*********************** 函数声明 ***********************/
void UART_init(void);
void SendByte(uint8_t dat);
void SendReply(uint8_t cmd, uint8_t status);
void Timer0_Init(void);

// EEPROM 操作函数
void IAP_Enable(void);
void IAP_Disable(void);
uint8_t EEPROM_ReadByte(uint16_t addr);
void EEPROM_EraseSector(uint16_t addr);
void EEPROM_WriteByte(uint16_t addr, uint8_t dat);
void SaveLightState(uint8_t state);
uint8_t LoadLightState(void);

/*********************** 主函数 ***********************/
void main(void)
{
    uint8_t addr, cmd;
    uint8_t need_reply;

    // I/O 初始化
    P1M0 |= 0x01;   // P1.0 推挽输出（支持高电平驱动）
    P1M1 &= ~0x01;
    P3M0 |= 0x80;   // P3.7 TxD 推挽
    P3M1 &= 0x7F;
    P3M0 &= ~0x40;  // P3.6 RxD 准双向
    P3M1 &= ~0x40;

    // 恢复上次灯状态（1=开灯/高电平，0=关灯/低电平）
    g_light_on = LoadLightState();
    STATE_LED = g_light_on ? 1 : 0;   // 高电平开灯

    UART_init();
    Timer0_Init();      // 1ms 定时器，用于延迟写入计时
    WDT_CONTR = 0x27;   // 看门狗 8s（提前开启，防止启动阶段卡死）
    EA = 1;             // 开总中断

    while(1)
    {
        WDT_CONTR = 0x10;   // 喂狗（只操作 bit4，不影响其他位）

        // 处理延迟写入（主循环中执行，避免在中断中擦写EEPROM）
        if(do_save)
        {
            do_save = 0;
            SaveLightState(pending_state);   // 真正写入 EEPROM
        }

        if(recv_ok)
        {
            // 临界区：复制缓冲区后立即清标志，避免被新帧打断
            ES = 0;             // 关 UART 中断
            addr = rx_buf[0];
            cmd  = rx_buf[1];
            recv_ok = 0;
            ES = 1;             // 开 UART 中断

            if(addr == DEVICE_ADDR || addr == 0xFF)
            {
                switch(cmd)
                {
                    case CMD_ON:
                        if(g_light_on == 0)
                        {
                            g_light_on = 1;
                            STATE_LED = 1;   // 高电平开灯
                            pending_state = g_light_on;
                            need_save = 1;
                            save_timer = SAVE_DELAY_MS;
                        }
                        break;
                    case CMD_OFF:
                        if(g_light_on == 1)
                        {
                            g_light_on = 0;
                            STATE_LED = 0;   // 低电平关灯
                            pending_state = g_light_on;
                            need_save = 1;
                            save_timer = SAVE_DELAY_MS;
                        }
                        break;
                    case CMD_TOGGLE:
                        g_light_on = !g_light_on;
                        STATE_LED = g_light_on ? 1 : 0;
                        pending_state = g_light_on;
                        need_save = 1;
                        save_timer = SAVE_DELAY_MS;
                        break;
                    case CMD_QUERY:
                        break;
                    default:
                        continue;
                }

                need_reply = 0;
                if(addr == DEVICE_ADDR) need_reply = 1;
                // 广播地址 0xFF 不回复，实现乐观更新
                // else if(addr == 0xFF && BROADCAST_REPLY) need_reply = 1;

                if(need_reply)
                {
                    SendReply(cmd, g_light_on);
                }
            }
        }
    }
}

/**************** UART 初始化 ****************/
void UART_init(void)
{
    P_SW1 = (P_SW1 & ~0xC0) | 0x40;
    SCON = 0x50;
    AUXR |= 0x40;
    AUXR &= 0xFE;
    TMOD &= 0x0F;
    TL1 = (65536 - (FOSC/4/BAUD));
    TH1 = (65536 - (FOSC/4/BAUD)) >> 8;
    TR1 = 1;
    ES = 1;
}

/**************** 定时器0初始化（1ms中断） ****************/
void Timer0_Init(void)
{
    AUXR |= 0x80;          // 定时器0 1T模式
    TMOD &= 0xF0;          // 16位自动重载
    TL0 = (65536 - (FOSC/1000));   // 1ms 重载值
    TH0 = (65536 - (FOSC/1000)) >> 8;
    TR0 = 1;
    ET0 = 1;               // 开启定时器0中断
}

/**************** 定时器0中断服务（1ms） ****************/
void Timer0_ISR() interrupt 1
{
    // 帧超时复位
    if(rx_timeout)
    {
        if(--rx_timeout == 0)
            rx_state = WAIT_HEAD1;
    }
    // 延迟写入计时
    if(need_save && save_timer)
    {
        save_timer--;
        if(save_timer == 0)
        {
            need_save = 0;
            do_save = 1;   // 通知主循环去执行保存
        }
    }
}

/**************** 发送单字节 ****************/
void SendByte(uint8_t dat)
{
    SBUF = dat;
    while(!TI);
    TI = 0;
}

/**************** 发送回复帧 ****************/
void SendReply(uint8_t cmd, uint8_t status)
{
    SendByte(FRAME_HEAD1);
    SendByte(FRAME_HEAD2);
    SendByte(DEVICE_ADDR);
    SendByte(cmd);
    SendByte(status);
    SendByte(FRAME_TAIL1);
    SendByte(FRAME_TAIL2);
}

/**************** 串口中断 ****************/
void UART_ISR() interrupt 4
{
    uint8_t dat;
    if(RI)
    {
        dat = SBUF;
        RI = 0;
        switch(rx_state)
        {
            case WAIT_HEAD1:
                if(dat == FRAME_HEAD1)
                {
                    rx_state = WAIT_HEAD2;
                    rx_timeout = RX_TIMEOUT_MS;  // 启动超时计时
                }
                break;
            case WAIT_HEAD2:
                if(dat == FRAME_HEAD2) rx_state = WAIT_ADDR;
                else if(dat == FRAME_HEAD1) { rx_timeout = RX_TIMEOUT_MS; }
                else { rx_state = WAIT_HEAD1; rx_timeout = 0; }
                break;
            case WAIT_ADDR:
                if(dat == DEVICE_ADDR || dat == 0xFF)
                {
                    rx_buf[0] = dat;
                    rx_state = WAIT_CMD;
                }
                else if(dat == FRAME_HEAD1) { rx_state = WAIT_HEAD2; rx_timeout = RX_TIMEOUT_MS; }
                else { rx_state = WAIT_HEAD1; rx_timeout = 0; }
                break;
            case WAIT_CMD:
                if(dat == CMD_ON || dat == CMD_OFF || dat == CMD_QUERY || dat == CMD_TOGGLE)
                {
                    rx_buf[1] = dat;
                    rx_state = WAIT_TAIL1;
                }
                else if(dat == FRAME_HEAD1) { rx_state = WAIT_HEAD2; rx_timeout = RX_TIMEOUT_MS; }
                else { rx_state = WAIT_HEAD1; rx_timeout = 0; }
                break;
            case WAIT_TAIL1:
                if(dat == FRAME_TAIL1) rx_state = WAIT_TAIL2;
                else if(dat == FRAME_HEAD1) { rx_state = WAIT_HEAD2; rx_timeout = RX_TIMEOUT_MS; }
                else { rx_state = WAIT_HEAD1; rx_timeout = 0; }
                break;
            case WAIT_TAIL2:
                if(dat == FRAME_TAIL2) recv_ok = 1;
                rx_state = WAIT_HEAD1;
                rx_timeout = 0;  // 帧完成，清超时
                break;
            default:
                rx_state = WAIT_HEAD1;
                rx_timeout = 0;
                break;
        }
    }
}

/*************** EEPROM 操作 (IAP) ***************/
void IAP_Enable(void)
{
    IAP_CONTR = 0x80;
    IAP_TPS = 11;   // 11.0592 MHz -> 11 (手册: FOSC/1000000)
}

void IAP_Disable(void)
{
    IAP_CONTR = 0x00;
    IAP_CMD = 0x00;
    IAP_TRIG = 0x00;
    IAP_ADDRH = 0x00;
    IAP_ADDRL = 0x00;
}

uint8_t EEPROM_ReadByte(uint16_t addr)
{
    uint8_t dat;
    uint8_t ea;
    ea = EA; EA = 0;        // 关中断保护IAP操作
    IAP_Enable();
    IAP_CMD = 0x01;
    IAP_ADDRL = addr & 0xFF;
    IAP_ADDRH = (addr >> 8) & 0xFF;
    IAP_TRIG = 0x5A;
    IAP_TRIG = 0xA5;
    _nop_();
    dat = IAP_DATA;
    IAP_Disable();
    EA = ea;                 // 恢复中断状态
    return dat;
}

void EEPROM_EraseSector(uint16_t addr)
{
    uint8_t ea;
    ea = EA; EA = 0;
    IAP_Enable();
    IAP_CMD = 0x03;
    IAP_ADDRL = addr & 0xFF;
    IAP_ADDRH = (addr >> 8) & 0xFF;
    IAP_TRIG = 0x5A;
    IAP_TRIG = 0xA5;
    _nop_();
    IAP_Disable();
    EA = ea;
}

void EEPROM_WriteByte(uint16_t addr, uint8_t dat)
{
    uint8_t ea;
    ea = EA; EA = 0;
    IAP_Enable();
    IAP_CMD = 0x02;
    IAP_ADDRL = addr & 0xFF;
    IAP_ADDRH = (addr >> 8) & 0xFF;
    IAP_DATA = dat;
    IAP_TRIG = 0x5A;
    IAP_TRIG = 0xA5;
    _nop_();
    IAP_Disable();
    EA = ea;
}

// EEPROM wear leveling：扇区内512个地址轮流写入，寿命提升512倍
// 格式：[有效标记0x5A][状态值] 占2字节，依次往后写，写满512字节后擦除重来
#define EEPROM_SECTOR_BASE  0x0000
#define EEPROM_SECTOR_SIZE  512
#define EEPROM_SLOT_SIZE    2

// 保存灯状态（wear leveling）
void SaveLightState(uint8_t state)
{
    uint16_t addr;
    uint8_t val;

    // 从扇区开头扫描，找到第一个空槽（0xFF=未写入）
    for(addr = EEPROM_SECTOR_BASE; addr < EEPROM_SECTOR_BASE + EEPROM_SECTOR_SIZE; addr += EEPROM_SLOT_SIZE)
    {
        val = EEPROM_ReadByte(addr);
        if(val == 0xFF) break;   // 找到空槽
    }

    // 找不到空槽，擦除整个扇区，从头开始
    if(addr >= EEPROM_SECTOR_BASE + EEPROM_SECTOR_SIZE)
    {
        EEPROM_EraseSector(EEPROM_SECTOR_BASE);
        addr = EEPROM_SECTOR_BASE;
    }

    // 写入新槽：标记 + 状态值
    EEPROM_WriteByte(addr, 0x5A);
    EEPROM_WriteByte(addr + 1, state);
}

// 读取灯状态，扫描最后一个有效槽
uint8_t LoadLightState(void)
{
    uint16_t addr;
    uint8_t last_state = DEFAULT_LIGHT_STATE;

    for(addr = EEPROM_SECTOR_BASE; addr < EEPROM_SECTOR_BASE + EEPROM_SECTOR_SIZE; addr += EEPROM_SLOT_SIZE)
    {
        if(EEPROM_ReadByte(addr) == 0x5A)
        {
            uint8_t val = EEPROM_ReadByte(addr + 1);
            // 严格只承认 0 或 1，丢弃非法值
            if(val == 0 || val == 1)
                last_state = val;
        }
        else
        {
            break;  // 后面都是空槽，不用继续扫
        }
    }
    return last_state;
}
