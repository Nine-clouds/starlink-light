/**************************************************************
 * 功能：1527遥控网关（STC15W4K32S4）
 * 硬件连接：
 *   UART1 (P3.6/ RxD1, P3.7/TxD1) <-> 语音模块
 *   UART2 (P1.0/RxD2, P1.1/TxD2) <-> ESP32
 *   UART3 (P0.0/RxD3, P0.1/TxD3) <-> HC-12
 *   UART4 (P0.2/RxD4)            <-  1527解码模块（只接收）
 * 功能说明：
 *   - UART1/UART2 <-> UART3 双向透传
 *   - 1527遥控触发 -> 查表映射 -> 通过HC-12发送翻转命令
 *   - 上电后广播查询所有灯状态
 *   - 支持ESP32通过UART2写入/读取1527映射表，断电EEPROM保存
 * 重复抑制：相同地址+键值，间隔<300ms则忽略（长按不输出），≥300ms或空闲后首帧则输出
 * 协议：
 *   1527：FD + AddrH + AddrL + Key + OSC + DF
 *   自定义控制帧：10 18 [地址] [命令] 18 10
 *     命令: 0x01开灯, 0x02关灯, 0x03查询, 0x04翻转
 *           0xA0写入映射表, 0xA1读取映射表
 *     地址: 0xFF广播（所有从机响应）
 *   映射表帧(ESP32->STC15): 10 18 FF A0 [count] [addr_h addr_l key dev_addr]×N 18 10
 *   映射表帧(STC15->ESP32): 10 18 FF A1 [count] [addr_h addr_l key dev_addr]×N 18 10
 **************************************************************/

#include "STC15Fxxxx.H"
#include <intrins.h>

#define MAIN_Fosc    22118400L   // 22.1184MHz（内部IRC）
#define BAUDRATE     9600

sbit LED = P1^7;                 // 指示灯，低电平点亮

/************************** 自定义帧协议 **************************/
#define FRAME_HEAD0     0x10
#define FRAME_HEAD1     0x18
#define FRAME_TAIL0     0x18
#define FRAME_TAIL1     0x10
#define CMD_ON          0x01    // 开灯
#define CMD_OFF         0x02    // 关灯
#define CMD_QUERY       0x03    // 查询状态
#define CMD_TOGGLE      0x04    // 翻转
#define CMD_SET_MAP     0xA0    // 写入1527映射表
#define CMD_GET_MAP     0xA1    // 读取1527映射表
#define ADDR_BROADCAST  0xFF    // 广播地址，所有从机响应
/**************************************************************/

/************************** 映射表配置 **************************/
#define MAP_MAX_ENTRIES  32      // 最大映射条数
#define MAP_ENTRY_SIZE   4       // 每条映射: addr_h + addr_l + key + dev_addr

struct map_entry {
    unsigned char addr_h;    // 遥控器地址高字节
    unsigned char addr_l;    // 遥控器地址低字节
    unsigned char rf_key;    // 遥控器键值
    unsigned char dev_addr;  // 对应设备地址
};

// 运行时映射表（xdata RAM，可修改）
struct map_entry xdata map_table[MAP_MAX_ENTRIES];
unsigned char map_count = 0;     // 当前映射条数

// 出厂默认映射表（code Flash，仅首次上电时复制到RAM）
struct map_entry const code default_map[] = {
    {0x68, 0x2D, 0x34, 0x01},
    {0x68, 0x2D, 0x32, 0x01},
    {0x08, 0xFB, 0xD8, 0x01},
    {0x68, 0x2D, 0x36, 0x01},
};
#define DEFAULT_MAP_SIZE  (sizeof(default_map) / sizeof(default_map[0]))

/************************** EEPROM 配置 **************************/
#define EEPROM_MAP_ADDR   0x0000    // 映射表在EEPROM中的起始地址
#define EEPROM_MAGIC_ADDR (0x0000 + MAP_MAX_ENTRIES * MAP_ENTRY_SIZE + 1)  // 魔数地址
#define EEPROM_MAGIC      0xA5      // 魔数，表示EEPROM已初始化
/**************************************************************/

#define RX_BUF_SIZE  128
#define TX_BUF_SIZE  128

// 各串口接收环形缓冲区
unsigned char xdata rx1_buf[RX_BUF_SIZE];   // UART1（语音模块）
volatile unsigned char rx1_head = 0, rx1_tail = 0;
unsigned char xdata rx2_buf[RX_BUF_SIZE];   // UART2（ESP32）
volatile unsigned char rx2_head = 0, rx2_tail = 0;
unsigned char xdata rx3_buf[RX_BUF_SIZE];   // UART3（HC-12）
volatile unsigned char rx3_head = 0, rx3_tail = 0;
// UART4 只接收1527，不参与透传

// 各串口发送环形缓冲区（中断驱动，不阻塞主循环）
unsigned char xdata tx1_buf[TX_BUF_SIZE];
volatile unsigned char tx1_head = 0, tx1_tail = 0;
volatile bit tx1_busy = 0;
unsigned char xdata tx2_buf[TX_BUF_SIZE];
volatile unsigned char tx2_head = 0, tx2_tail = 0;
volatile bit tx2_busy = 0;
unsigned char xdata tx3_buf[TX_BUF_SIZE];
volatile unsigned char tx3_head = 0, tx3_tail = 0;
volatile bit tx3_busy = 0;
// UART4 只接收，无发送

// 1527帧解析状态机
enum { WAIT_HEAD, WAIT_ADDR_H, WAIT_ADDR_L, WAIT_KEY, WAIT_OSC, WAIT_TAIL } parse_state = WAIT_HEAD;
unsigned char frame_addr_h, frame_addr_l, frame_key;
volatile unsigned char parse_timeout = 0;   // 状态机超时计数(1ms递减)

// 1527帧接收队列（ISR→主循环）
#define RF_Q_SIZE  4
unsigned int xdata rf_q_addr[RF_Q_SIZE];
unsigned char xdata rf_q_key[RF_Q_SIZE];
volatile unsigned char rf_q_head = 0, rf_q_tail = 0;

// 1527映射转发队列
#define MAP_QUEUE_SIZE  8
unsigned char map_queue[MAP_QUEUE_SIZE];
volatile unsigned char map_q_head = 0, map_q_tail = 0;

// UART2(ESP32) 帧解析状态机
enum { U2_IDLE, U2_HEAD1, U2_HEAD2, U2_ADDR, U2_CMD, U2_MAP_COUNT,
       U2_MAP_DATA, U2_TAIL1, U2_TAIL2 } u2_state = U2_IDLE;
unsigned char u2_addr, u2_cmd;
unsigned char u2_map_count, u2_map_idx;
unsigned char xdata u2_map_buf[MAP_MAX_ENTRIES * MAP_ENTRY_SIZE];
volatile unsigned char u2_parse_timeout = 0;

// LED非闪烁定时
volatile unsigned char led_timer = 0;

// 1ms滴答计数(用于精确delay_ms和重复抑制)
volatile unsigned int tick_ms = 0;

// ==================== 重复抑制变量 ====================
volatile unsigned int last_rf_addr = 0;
volatile unsigned char last_rf_key = 0;
volatile unsigned int last_forward_tick = 0;
#define REPEAT_MIN_INTERVAL_MS  300   // 相同编码最小间隔300ms，小于此值视为长按忽略
volatile bit rf_idle = 1;             // 1=1527空闲，下一帧无条件接受（解决tick_ms溢出问题）

void UART1_Init(void);
void UART2_Init(void);
void UART3_Init(void);
void UART4_Init(void);
void Timer0_Init(void);
void UART1_SendByte(unsigned char dat);
void UART2_SendByte(unsigned char dat);
void UART3_SendByte(unsigned char dat);
void UART3_SendCmd(unsigned char addr, unsigned char cmd);
void UART2_SendMap(void);
void delay_ms(unsigned int ms);
void process_1527_frame(unsigned int addr, unsigned char key);
void forward_rx_data(void);
void process_uart2_data(void);
void eeprom_load_map(void);
void eeprom_save_map(void);
void iap_idle(void);
unsigned char iap_read_byte(unsigned int addr);
void iap_program_byte(unsigned int addr, unsigned char dat);
void iap_erase_sector(unsigned int sector);

void main(void)
{
    // 所有I/O口初始化为准双向
    P0M1 = 0; P0M0 = 0;
    P1M1 = 0; P1M0 = 0;
    P2M1 = 0; P2M0 = 0;
    P3M1 = 0; P3M0 = 0;
    P4M1 = 0; P4M0 = 0;
    P5M1 = 0; P5M0 = 0;

    LED = 1;

    P_SW2 |= 0x80;          // EAXFR=1，允许访问扩展SFR(全局开启，不再关闭)

    Timer0_Init();   // 1ms定时器(状态机超时+LED+delay用)
    UART1_Init();   // 语音模块 (P3.6/P3.7)
    UART2_Init();   // ESP32 (P1.0/P1.1)
    UART3_Init();   // HC-12 (P0.0/P0.1)
    UART4_Init();   // 1527模块 (P0.2 only)

    WDT_CONTR = 0x35;   // 看门狗：预分频256，约4.5s超时

    EA = 1;

    // 从EEPROM加载映射表（首次上电则写入默认值）
    eeprom_load_map();

    // 上电提示（通过HC-12发送）
    UART3_SendByte('O'); UART3_SendByte('K'); UART3_SendByte('\r'); UART3_SendByte('\n');

    // 不再自动广播查询，由ESP32 MQTT连接后主动发QUERY
    // （HC-12半双工，同时发送会丢失回复）

    while (1)
    {
        // 处理1527帧队列（从ISR接收，主循环中做映射+重复抑制）
        if (rf_q_head != rf_q_tail)
        {
            process_1527_frame(rf_q_addr[rf_q_head], rf_q_key[rf_q_head]);
            rf_q_head = (rf_q_head + 1) % RF_Q_SIZE;
        }
        // 处理1527映射转发队列
        if (map_q_head != map_q_tail)
        {
            UART3_SendCmd(map_queue[map_q_head], CMD_TOGGLE);
            map_q_head = (map_q_head + 1) % MAP_QUEUE_SIZE;
            LED = 0;
            led_timer = 20;     // 20ms后自动关灯，不阻塞
        }
        // 处理UART2(ESP32)接收的数据
        process_uart2_data();
        forward_rx_data();
        WDT_CONTR = 0x35;      // 喂狗
    }
}

// 透传转发：HC-12(UART3)与UART1/UART2互通
// 注意：UART2(ESP32)数据由process_uart2_data()处理，不再直接透传
void forward_rx_data(void)
{
    // UART3 (HC-12) -> UART1/UART2（透传，不做帧解析）
    while (rx3_head != rx3_tail)
    {
        unsigned char dat = rx3_buf[rx3_tail++];
        if (rx3_tail >= RX_BUF_SIZE) rx3_tail = 0;
        UART1_SendByte(dat);
        UART2_SendByte(dat);
    }
    // UART1 (语音模块) -> UART3 (HC-12)
    while (rx1_head != rx1_tail)
    {
        unsigned char dat = rx1_buf[rx1_tail++];
        if (rx1_tail >= RX_BUF_SIZE) rx1_tail = 0;
        UART3_SendByte(dat);
    }
    // UART2(ESP32)数据不再透传，由process_uart2_data()解析映射表帧
    // 普通控制帧仍需转发到HC-12，在process_uart2_data中处理
}

// 处理1527帧（含重复抑制）
void process_1527_frame(unsigned int addr, unsigned char key)
{
    unsigned char i;
    unsigned char next_tail;
    unsigned int now_tick;

    EA = 0;
    now_tick = tick_ms;
    EA = 1;

    // 空闲后首帧无条件接受（避免tick_ms溢出导致误抑制）
    if (!rf_idle)
    {
        // 相同编码且时间间隔小于阈值 → 忽略（长按不输出）
        if (addr == last_rf_addr && key == last_rf_key)
        {
            if ((unsigned int)(now_tick - last_forward_tick) < REPEAT_MIN_INTERVAL_MS)
                return;
        }
    }
    rf_idle = 0;

    // 查找映射表
    for (i = 0; i < map_count; i++)
    {
        if (map_table[i].addr_h == (addr >> 8) &&
            map_table[i].addr_l == (addr & 0xFF) &&
            map_table[i].rf_key == key)
        {
            // 更新记录（地址、键值、时间）
            last_rf_addr = addr;
            last_rf_key = key;
            last_forward_tick = now_tick;

            next_tail = (map_q_tail + 1) % MAP_QUEUE_SIZE;
            if (next_tail != map_q_head)  // 队列不满才入队
            {
                map_queue[map_q_tail] = map_table[i].dev_addr;
                map_q_tail = next_tail;
            }
            break;
        }
    }
}

//====================== 串口中断服务 ======================
void UART1_int (void) interrupt UART1_VECTOR
{
    if (RI)
    {
        unsigned char dat, next_head;
        RI = 0;
        dat = SBUF;
        next_head = rx1_head + 1;
        if (next_head >= RX_BUF_SIZE) next_head = 0;
        if (next_head != rx1_tail)
        {
            rx1_buf[rx1_head] = dat;
            rx1_head = next_head;
        }
    }
    if (TI)
    {
        TI = 0;
        if (tx1_head != tx1_tail)
        {
            SBUF = tx1_buf[tx1_tail++];
            if (tx1_tail >= TX_BUF_SIZE) tx1_tail = 0;
        }
        else
        {
            tx1_busy = 0;
        }
    }
}

void UART2_int (void) interrupt UART2_VECTOR
{
    if (S2CON & 0x01)
    {
        unsigned char dat, next_head;
        dat = S2BUF;
        S2CON &= ~0x01;
        next_head = rx2_head + 1;
        if (next_head >= RX_BUF_SIZE) next_head = 0;
        if (next_head != rx2_tail)
        {
            rx2_buf[rx2_head] = dat;
            rx2_head = next_head;
        }
    }
    if (S2CON & 0x02)
    {
        S2CON &= ~0x02;
        if (tx2_head != tx2_tail)
        {
            S2BUF = tx2_buf[tx2_tail++];
            if (tx2_tail >= TX_BUF_SIZE) tx2_tail = 0;
        }
        else
        {
            tx2_busy = 0;
        }
    }
}

void UART3_int (void) interrupt UART3_VECTOR
{
    if (S3CON & 0x01)
    {
        unsigned char dat, next_head;
        dat = S3BUF;
        S3CON &= ~0x01;
        next_head = rx3_head + 1;
        if (next_head >= RX_BUF_SIZE) next_head = 0;
        if (next_head != rx3_tail)
        {
            rx3_buf[rx3_head] = dat;
            rx3_head = next_head;
        }
    }
    if (S3CON & 0x02)
    {
        S3CON &= ~0x02;
        if (tx3_head != tx3_tail)
        {
            S3BUF = tx3_buf[tx3_tail++];
            if (tx3_tail >= TX_BUF_SIZE) tx3_tail = 0;
        }
        else
        {
            tx3_busy = 0;
        }
    }
}

void UART4_int (void) interrupt UART4_VECTOR
{
    unsigned char dat;
    if (S4CON & 0x01)   // RI4
    {
        dat = S4BUF;
        S4CON &= ~0x01;
        parse_timeout = 10;  // 刷新超时，约10ms
        // 1527帧解析
        switch (parse_state)
        {
            case WAIT_HEAD:
                if (dat == 0xFD) parse_state = WAIT_ADDR_H;
                break;
            case WAIT_ADDR_H:
                frame_addr_h = dat;
                parse_state = WAIT_ADDR_L;
                break;
            case WAIT_ADDR_L:
                frame_addr_l = dat;
                parse_state = WAIT_KEY;
                break;
            case WAIT_KEY:
                frame_key = dat;
                parse_state = WAIT_OSC;
                break;
            case WAIT_OSC:
                parse_state = WAIT_TAIL;
                break;
            case WAIT_TAIL:
                if (dat == 0xDF)
                {
                    unsigned char next_tail = (rf_q_tail + 1) % RF_Q_SIZE;
                    if (next_tail != rf_q_head)
                    {
                        rf_q_addr[rf_q_tail] = (frame_addr_h << 8) | frame_addr_l;
                        rf_q_key[rf_q_tail] = frame_key;
                        rf_q_tail = next_tail;
                    }
                }
                parse_state = WAIT_HEAD;
                break;
            default:
                parse_state = WAIT_HEAD;
                break;
        }
    }
    // 不处理TI4（UART4不发送）
}

//====================== 发送函数（中断驱动，不阻塞）======================
void UART1_SendByte(unsigned char dat)
{
    unsigned char next_head = tx1_head + 1;
    if (next_head >= TX_BUF_SIZE) next_head = 0;
    while (next_head == tx1_tail);  // 缓冲区满则等待（极少发生）
    EA = 0;
    tx1_buf[tx1_head] = dat;
    tx1_head = next_head;
    if (!tx1_busy)
    {
        tx1_busy = 1;
        SBUF = tx1_buf[tx1_tail++];
        if (tx1_tail >= TX_BUF_SIZE) tx1_tail = 0;
    }
    EA = 1;
}

void UART2_SendByte(unsigned char dat)
{
    unsigned char next_head = tx2_head + 1;
    if (next_head >= TX_BUF_SIZE) next_head = 0;
    while (next_head == tx2_tail);
    EA = 0;
    tx2_buf[tx2_head] = dat;
    tx2_head = next_head;
    if (!tx2_busy)
    {
        tx2_busy = 1;
        S2BUF = tx2_buf[tx2_tail++];
        if (tx2_tail >= TX_BUF_SIZE) tx2_tail = 0;
    }
    EA = 1;
}

void UART3_SendByte(unsigned char dat)
{
    unsigned char next_head = tx3_head + 1;
    if (next_head >= TX_BUF_SIZE) next_head = 0;
    while (next_head == tx3_tail);
    EA = 0;
    tx3_buf[tx3_head] = dat;
    tx3_head = next_head;
    if (!tx3_busy)
    {
        tx3_busy = 1;
        S3BUF = tx3_buf[tx3_tail++];
        if (tx3_tail >= TX_BUF_SIZE) tx3_tail = 0;
    }
    EA = 1;
}

// 发送命令帧(6字节): 10 18 Addr Cmd 18 10
void UART3_SendCmd(unsigned char addr, unsigned char cmd)
{
    UART3_SendByte(FRAME_HEAD0);
    UART3_SendByte(FRAME_HEAD1);
    UART3_SendByte(addr);
    UART3_SendByte(cmd);
    UART3_SendByte(FRAME_TAIL0);
    UART3_SendByte(FRAME_TAIL1);
}

// UART2回传映射表: 10 18 FF A1 [count] [addr_h addr_l key dev_addr]×N 18 10
void UART2_SendMap(void)
{
    unsigned char i;
    UART2_SendByte(FRAME_HEAD0);    // 0x10
    UART2_SendByte(FRAME_HEAD1);    // 0x18
    UART2_SendByte(ADDR_BROADCAST); // 0xFF
    UART2_SendByte(CMD_GET_MAP);    // 0xA1
    UART2_SendByte(map_count);
    for (i = 0; i < map_count; i++)
    {
        UART2_SendByte(map_table[i].addr_h);
        UART2_SendByte(map_table[i].addr_l);
        UART2_SendByte(map_table[i].rf_key);
        UART2_SendByte(map_table[i].dev_addr);
    }
    UART2_SendByte(FRAME_TAIL0);    // 0x18
    UART2_SendByte(FRAME_TAIL1);    // 0x10
}

//====================== 串口初始化 ======================
// UART1: P3.6/RxD, P3.7/TxD (语音模块)
void UART1_Init(void)
{
    TR1 = 0;
    AUXR &= ~0x01;          // S1 BRT Use Timer1
    AUXR |=  (1<<6);        // Timer1 1T mode
    TMOD &= ~0x30;          // Timer1 16位自动重载
    TH1 = (unsigned char)((65536UL - (MAIN_Fosc / 4) / BAUDRATE) / 256);
    TL1 = (unsigned char)((65536UL - (MAIN_Fosc / 4) / BAUDRATE) % 256);
    ET1 = 0;
    TR1 = 1;
    SCON = 0x50;            // 模式1，允许接收
    PS = 0;
    ES = 1;
    REN = 1;
    // 选择引脚为 P3.6/P3.7
    P_SW1 = (P_SW1 & 0x3F) | 0x40;    // bit[7:6]=01: P3.6(RxD)/P3.7(TxD)
}

// UART2: P1.0/RxD2, P1.1/TxD2 (ESP32)
void UART2_Init(void)
{
    AUXR &= ~(1<<4);        // Timer2 stop
    AUXR &= ~(1<<3);        // Timer2 as Timer
    AUXR |=  (1<<2);        // Timer2 1T mode
    TH2 = (unsigned char)((65536UL - (MAIN_Fosc / 4) / BAUDRATE) / 256);
    TL2 = (unsigned char)((65536UL - (MAIN_Fosc / 4) / BAUDRATE) % 256);
    IE2 &= ~(1<<2);
    AUXR |=  (1<<4);        // Timer2 run
    S2CON = 0x50;
    IE2 |= 1;               // 开UART2中断
    P_SW2 = (P_SW2 & ~0x01) | 0x80;  // UART2引脚: P1.0/P1.1, 保持EAXFR
}

// UART3: P0.0/RxD3, P0.1/TxD3 (HC-12)
void UART3_Init(void)
{
    S3CON |= (1<<6);        // S3ST3=1, 波特率发生器选Timer3
    T4T3M &= 0xF0;          // 停止Timer3, 清除Timer3控制位(bit3-0)，保留Timer4位(bit7-4)
    IE2  &= ~(1<<5);        // 禁止Timer3中断
    T4T3M |=  (1<<1);       // Timer3 1T模式
    T4T3M &= ~(1<<2);       // Timer3 定时模式
    T4T3M &= ~0x01;         // Timer3 不输出时钟
    TH3 = (65536UL - (MAIN_Fosc / 4) / BAUDRATE) / 256;
    TL3 = (65536UL - (MAIN_Fosc / 4) / BAUDRATE) % 256;
    T4T3M |=  (1<<3);       // Timer3 开始运行
    S3CON &= ~(1<<5);       // 禁止多机通讯
    S3CON &= ~(1<<7);       // 8位数据, 1位起始位, 1位停止位
    IE2   |=  (1<<3);       // 允许UART3中断
    S3CON |=  (1<<4);       // 允许接收
    P_SW2 = (P_SW2 & ~0x02) | 0x80;  // UART3引脚: P0.0/P0.1, 保持EAXFR
}

// UART4: P0.2/RxD4, P0.3/TxD4 (1527模块，只用RxD4)
void UART4_Init(void)
{
    S4CON |= (1<<6);        // S4ST4=1, 波特率发生器选Timer4
    T4T3M &= 0x0F;          // 停止Timer4, 清除Timer4控制位(bit7-4)，保留Timer3位(bit3-0)
    IE2   &= ~(1<<6);       // 禁止Timer4中断
    T4T3M |=  (1<<5);       // Timer4 1T模式
    T4T3M &= ~(1<<6);       // Timer4 定时模式
    T4T3M &= ~(1<<4);       // Timer4 不输出时钟
    TH4 = (65536UL - (MAIN_Fosc / 4) / BAUDRATE) / 256;
    TL4 = (65536UL - (MAIN_Fosc / 4) / BAUDRATE) % 256;
    T4T3M |=  (1<<7);       // Timer4 开始运行
    S4CON &= ~(1<<5);       // 禁止多机通讯
    S4CON &= ~(1<<7);       // 8位数据, 1位起始位, 1位停止位
    IE2   |=  (1<<4);       // 允许UART4中断
    S4CON |=  (1<<4);       // 允许接收
    P_SW2 = (P_SW2 & ~0x04) | 0x80;  // UART4引脚: P0.2/P0.3, 保持EAXFR
}

// 基于Timer0的精确延时
void delay_ms(unsigned int ms)
{
    unsigned int target;
    EA = 0;
    target = tick_ms + ms;
    EA = 1;
    while (1)
    {
        unsigned int cur;
        EA = 0;
        cur = tick_ms;
        EA = 1;
        if ((unsigned int)(cur - (target - ms)) >= ms) break;
    }
}

//====================== Timer0 1ms定时 ======================
void Timer0_Init(void)
{
    AUXR |= 0x80;           // Timer0 1T模式
    TMOD &= ~0x0F;          // Timer0 16位自动重载
    TH0 = (unsigned char)((65536UL - MAIN_Fosc / 1000) >> 8);  // 1ms
    TL0 = (unsigned char)((65536UL - MAIN_Fosc / 1000));
    ET0 = 1;
    TR0 = 1;
}

void Timer0_int(void) interrupt 1
{
    tick_ms++;              // 1ms滴答
    // 1527状态机超时复位
    if (parse_timeout)
    {
        if (--parse_timeout == 0)
        {
            parse_state = WAIT_HEAD;
            rf_idle = 1;    // 空闲后首帧无条件接受，避免tick_ms溢出导致误抑制
        }
    }
    // UART2帧解析超时复位
    if (u2_parse_timeout)
    {
        if (--u2_parse_timeout == 0)
            u2_state = U2_IDLE;
    }
    // LED非阻塞关灯
    if (led_timer)
    {
        if (--led_timer == 0)
            LED = 1;
    }
}

//====================== IAP EEPROM 操作 ======================
void iap_idle(void)
{
    IAP_CONTR = 0;         // 关闭IAP
    IAP_CMD = 0;           // 清命令
    IAP_TRIG = 0;          // 清触发
    IAP_ADDRH = 0xFF;      // 地址指向非EEPROM区域
    IAP_ADDRL = 0xFF;
}

unsigned char iap_read_byte(unsigned int addr)
{
    unsigned char dat;
    IAP_CONTR = 0x81;      // IAPEN=1, 等待时间=0 (22MHz)
    IAP_CMD = 1;            // 读命令
    IAP_ADDRL = addr & 0xFF;
    IAP_ADDRH = (addr >> 8) & 0xFF;
    IAP_TRIG = 0x5A;       // 触发序列
    IAP_TRIG = 0xA5;
    _nop_();
    dat = IAP_DATA;
    iap_idle();
    return dat;
}

void iap_program_byte(unsigned int addr, unsigned char dat)
{
    IAP_CONTR = 0x81;      // IAPEN=1, 等待时间=0
    IAP_CMD = 2;            // 写命令
    IAP_ADDRL = addr & 0xFF;
    IAP_ADDRH = (addr >> 8) & 0xFF;
    IAP_DATA = dat;
    IAP_TRIG = 0x5A;
    IAP_TRIG = 0xA5;
    _nop_();
    iap_idle();
}

void iap_erase_sector(unsigned int sector_addr)
{
    IAP_CONTR = 0x81;      // IAPEN=1, 等待时间=0
    IAP_CMD = 3;            // 擦除命令
    IAP_ADDRL = sector_addr & 0xFF;
    IAP_ADDRH = (sector_addr >> 8) & 0xFF;
    IAP_TRIG = 0x5A;
    IAP_TRIG = 0xA5;
    _nop_();
    iap_idle();
}

// 从EEPROM加载映射表到RAM
void eeprom_load_map(void)
{
    unsigned char i;
    unsigned char magic;
    unsigned int addr;

    magic = iap_read_byte(EEPROM_MAGIC_ADDR);

    if (magic == EEPROM_MAGIC)
    {
        // EEPROM已初始化，读取映射表
        map_count = iap_read_byte(EEPROM_MAP_ADDR);
        if (map_count > MAP_MAX_ENTRIES) map_count = 0;

        addr = EEPROM_MAP_ADDR + 1;
        for (i = 0; i < map_count; i++)
        {
            map_table[i].addr_h  = iap_read_byte(addr++);
            map_table[i].addr_l  = iap_read_byte(addr++);
            map_table[i].rf_key  = iap_read_byte(addr++);
            map_table[i].dev_addr = iap_read_byte(addr++);
        }
    }
    else
    {
        // 首次上电，从Flash复制默认表到RAM，再写入EEPROM
        map_count = DEFAULT_MAP_SIZE;
        for (i = 0; i < DEFAULT_MAP_SIZE; i++)
        {
            map_table[i] = default_map[i];
        }
        eeprom_save_map();
    }
}

// 保存映射表到EEPROM（关中断保护IAP操作）
void eeprom_save_map(void)
{
    unsigned char i;
    unsigned int addr;
    bit ea_save;

    ea_save = EA;
    EA = 0;                     // 关中断，保护IAP操作

    // 先擦除扇区（512字节扇区，地址0开始）
    iap_erase_sector(EEPROM_MAP_ADDR);

    // 写入映射条数
    iap_program_byte(EEPROM_MAP_ADDR, map_count);

    // 写入映射表数据
    addr = EEPROM_MAP_ADDR + 1;
    for (i = 0; i < map_count; i++)
    {
        iap_program_byte(addr++, map_table[i].addr_h);
        iap_program_byte(addr++, map_table[i].addr_l);
        iap_program_byte(addr++, map_table[i].rf_key);
        iap_program_byte(addr++, map_table[i].dev_addr);
    }

    // 写入魔数标记
    iap_program_byte(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);

    EA = ea_save;               // 恢复中断
}

//====================== UART2(ESP32) 帧解析 ======================
void process_uart2_data(void)
{
    unsigned char dat;
    unsigned char i;
    static bit u2_is_map = 0;   // 标记当前帧是否为映射表帧（static保持跨状态）

    while (rx2_head != rx2_tail)
    {
        dat = rx2_buf[rx2_tail++];
        if (rx2_tail >= RX_BUF_SIZE) rx2_tail = 0;

        u2_parse_timeout = 10;  // 10ms超时

        switch (u2_state)
        {
        case U2_IDLE:
            if (dat == FRAME_HEAD0)
                u2_state = U2_HEAD1;
            break;
        case U2_HEAD1:
            u2_state = (dat == FRAME_HEAD1) ? U2_HEAD2 : U2_IDLE;
            break;
        case U2_HEAD2:
            u2_addr = dat;
            u2_state = U2_ADDR;
            break;
        case U2_ADDR:
            u2_cmd = dat;
            if (dat == CMD_SET_MAP && u2_addr == ADDR_BROADCAST)
            {
                u2_is_map = 1;
                u2_state = U2_MAP_COUNT;
                u2_map_idx = 0;
            }
            else if (dat == CMD_GET_MAP && u2_addr == ADDR_BROADCAST)
            {
                u2_is_map = 0;
                // 回传映射表给ESP32
                UART2_SendMap();
                // 仍需消费尾部 18 10
                u2_state = U2_CMD;
            }
            else
            {
                u2_is_map = 0;
                // 普通控制命令，转发到HC-12
                UART3_SendCmd(u2_addr, u2_cmd);
                u2_state = U2_CMD;
            }
            break;
        case U2_CMD:
            // 普通帧/GET_MAP帧的尾部第一个字节 0x18
            u2_state = (dat == FRAME_TAIL0) ? U2_TAIL1 : U2_IDLE;
            break;
        case U2_MAP_COUNT:
            u2_map_count = dat;
            u2_map_idx = 0;
            if (u2_map_count == 0)
                u2_state = U2_TAIL1;
            else if (u2_map_count > MAP_MAX_ENTRIES)
                u2_state = U2_IDLE;
            else
                u2_state = U2_MAP_DATA;
            break;
        case U2_MAP_DATA:
            u2_map_buf[u2_map_idx++] = dat;
            if (u2_map_idx >= u2_map_count * MAP_ENTRY_SIZE)
                u2_state = U2_TAIL1;
            break;
        case U2_TAIL1:
            u2_state = (dat == FRAME_TAIL0) ? U2_TAIL2 : U2_IDLE;
            break;
        case U2_TAIL2:
            if (dat == FRAME_TAIL1)
            {
                if (u2_is_map)
                {
                    // 映射表接收完成，更新RAM
                    map_count = u2_map_count;
                    for (i = 0; i < map_count; i++)
                    {
                        map_table[i].addr_h   = u2_map_buf[i * MAP_ENTRY_SIZE + 0];
                        map_table[i].addr_l   = u2_map_buf[i * MAP_ENTRY_SIZE + 1];
                        map_table[i].rf_key   = u2_map_buf[i * MAP_ENTRY_SIZE + 2];
                        map_table[i].dev_addr = u2_map_buf[i * MAP_ENTRY_SIZE + 3];
                    }
                    // 保存到EEPROM
                    eeprom_save_map();

                    LED = 0;
                    led_timer = 100;  // 100ms指示灯

                    // 回传确认给ESP32
                    UART2_SendMap();
                }
                // 普通命令帧尾部已完成（已在U2_ADDR转发，无需额外处理）
            }
            u2_state = U2_IDLE;
            break;
        default:
            u2_state = U2_IDLE;
            break;
        }
    }
}
