#include "sdcard.h"

static uint8_t sd_high_capacity;

static void SPI_transmit_receive(uint8_t* tx, uint8_t* rx, size_t buff_size)
{
  HAL_SPI_TransmitReceive(&SD_SPI_PORT, tx, rx, buff_size, SD_TIMEOUT);
}

static void SPI_transmit(uint8_t* tx, size_t buff_size)
{
  HAL_SPI_Transmit(&SD_SPI_PORT, tx, buff_size, SD_TIMEOUT);
}

static void SD_read_bytes(uint8_t *buff, size_t buff_size)
{
  // make sure FF is transmitted during receive
  uint8_t tx = 0xFF;
  while (buff_size > 0)
  {
    SPI_transmit_receive(&tx, buff, 1);
    buff++;
    buff_size--;
  }

}

static void SD_select()
{
  HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_RESET);
  delay_us(10); // entry guard time for some SD cards
}

static void SD_unselect()
{
  HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET);
  delay_us(10); // exit guard time for some SD cards
}

static void SD_unselect_purge()
{
  SD_unselect();
  uint8_t tx = 0xFF;
  SPI_transmit(&tx, 1);
}

static HAL_StatusTypeDef SD_wait_not_busy()
{
  uint32_t start_time = HAL_GetTick();
  uint8_t busy;
  do
  {
    if (HAL_GetTick() >= start_time + SD_TIMEOUT)
      return HAL_TIMEOUT;
    SD_unselect();
    SD_select();
    SD_read_bytes(&busy, sizeof(busy));
  } while (busy != 0xFF);
  return HAL_OK;
}

static HAL_StatusTypeDef SD_read_r1(uint8_t *r1)
{
  /*
   * R1: 0abcdefg
   *     |||||||+- 0th bit (g): Card is in idle state
   *     ||||||+-- 1th bit (f): Erase sequence cleared
   *     |||||+--- 2th bit (e): Illegal command detected
   *     ||||+---- 3th bit (d): CRC check error
   *     |||+----- 4th bit (c): Error in the sequence of erase commands
   *     ||+------ 5th bit (b): Misaligned address used in command
   *     |+------- 6th bit (a): Command argument outside allowed range
   *     +-------- 7th bit is always zero
   */
  uint8_t tx = 0xFF;
  uint32_t start_time = HAL_GetTick();
  do
  {
    SPI_transmit_receive(&tx, r1, sizeof(uint8_t));
    if (HAL_GetTick() >= start_time + SD_TIMEOUT)
      return HAL_TIMEOUT;
  } while (*r1 & (1 << 7));
  return HAL_OK;
}

static HAL_StatusTypeDef SD_read_rx(uint8_t *rx, uint8_t x)
{
  HAL_StatusTypeDef r;
  int i;
  uint8_t tx = 0xFF;
  uint32_t start_time = HAL_GetTick();
  r = SD_read_r1(rx);
  if (r != HAL_OK)
    return r;
  rx++;
  for (i = 0; i < x; i++)
  {
    SPI_transmit_receive(&tx, rx, sizeof(uint8_t));
    if (HAL_GetTick() >= start_time + SD_TIMEOUT)
      return HAL_TIMEOUT;
    rx++;
  }
  return HAL_OK;
}

static HAL_StatusTypeDef SD_read_r3(uint8_t *r3)
{
  return SD_read_rx(r3, 4);
}

static HAL_StatusTypeDef SD_read_r7(uint8_t *r7)
{
  return SD_read_rx(r7, 4);
}

static void SD_send_cmd(uint8_t command, uint32_t arg, uint8_t crc)
{
  uint8_t cmd[] = { 0x40 | command, (arg >> 24) & 0xFF, (arg >> 16) & 0xFF, (arg >> 8) & 0xFF, arg & 0xFF, (crc << 1) | 1 };
  SD_select();
  SPI_transmit((uint8_t*) cmd, sizeof(cmd));
}

static HAL_StatusTypeDef SD_wait_data_token()
{
  uint8_t fb;
  uint8_t tx = 0xFF; // make sure FF is transmitted during receive
  uint32_t start_time = HAL_GetTick();
  while (1)
  {
    SPI_transmit_receive(&tx, &fb, sizeof(fb));
    if (fb == SD_DATA_TOKEN)
      break;
    if (fb != 0xFF)
      return HAL_ERROR;
    if (HAL_GetTick() >= start_time + SD_TIMEOUT)
      return HAL_TIMEOUT;
  }
  return HAL_OK;
}

HAL_StatusTypeDef SD_init_app_op_cond(uint32_t arg)
{
  HAL_StatusTypeDef r;
  int i;
  uint8_t r1;
  for (i = 0; i < SD_IDLE_RETRY_COUNT; i++)
  {
    // CMD55 (APP_CMD) before any ACMD command
    SD_send_cmd(55, 0, 0xFF);
    r = SD_read_r1(&r1);
    SD_unselect_purge();
    if (r != HAL_OK)
      return r;
    // ACMD41 - send operating condition
    SD_send_cmd(41, arg, 0xFF);
    r = SD_read_r1(&r1);
    SD_unselect_purge();
    if (r != HAL_OK)
      return r;
    if (r1 == 0x00)
      return HAL_OK; // initialization finished
  }
  return HAL_ERROR;
}

HAL_StatusTypeDef SD_init()
{
  HAL_StatusTypeDef r;
  uint8_t r1;
  uint8_t r3[5];
  uint8_t r7[5];
  int i;

  SD_select();
  SD_unselect();
  HAL_Delay(1);

  // 80 clock pulses to enter SPI mode
  uint8_t high = 0xFF;
  for (i = 0; i < 10; i++)
  {
    SPI_transmit(&high, sizeof(high));
  }

  // CMD0 - reset card
  for (i = 0; ; i++)
  {
    SD_send_cmd(0, 0x00000000, 0x4A);
    r = SD_read_r1(&r1);
    SD_unselect_purge();
    if (r != HAL_OK)
      return r;
    if (r1 == SD_R1_IDLE)
      break;
    if (i == SD_IDLE_RETRY_COUNT - 1)
      return HAL_ERROR;
  }

  /*
   * CMD8 - send interface condition
   *
   * 0th byte: R1 response
   *
   * 1st byte: Command version
   * rrrrmnop (7th bit to 0th bit)
   * ||||||||
   * |||||+++-- 3th to 0th (m-p): Command version. It corresponds to the command version in the argument of the CMD8 command.
   * ++++------ Reserved
   *
   * 2nd byte: Reserved and always 0
   *
   * 3nd byte: Reserved and always 0
   *
   * 4rd byte: Voltage acceptance
   * rrrrrvwx (7th bit to 0th bit)
   * ||||||||
   * |||||+++-- 3th to 0th bits (x-u): Voltage acceptance (VHS). It echoes back the VHS field in the argument of the CMD8 command.
   * |||||        0b0001: The card supports 2.7-3.6V range.
   * |||||        0b0010: The card supports low voltage range 1.65-1.95V.
   * |||||        0b0100 and 0b1000: Reserved for future use.
   * +++++----- Reserved
   *
   * 5th byte: Check pattern echo
   * yzABCDEF (7th bit to 0th bit)
   * ||||||||
   * ++++++++-- 7th to 0th bits (F-y): Echo of check pattern. This field is an echo-back of the check pattern set in the argument of the CMD8 command.
   */
  SD_send_cmd(8, 0x000001AA, 0x43); // request 2.7-3.63v
  r = SD_read_r7(r7);
  SD_unselect_purge();
  if (r != HAL_OK)
    return r;
  if (r7[0] & SD_R1_ILLEGAL_COMMAND)
  {
    // command not supported - old SD card version
    sd_high_capacity = 0;
    r = SD_init_app_op_cond(0);
    if (r != HAL_OK)
      return r;
  } else if (r7[0] == SD_R1_IDLE)
  {
    // new version
    // check that voltage accepted and echo byte
    if (((r7[3] & 0x01) != 1) || (r7[4] != 0xAA))
      return HAL_ERROR;
    r = SD_init_app_op_cond(0x40000000);
    if (r != HAL_OK)
      return r;
    /*
     * CMD58 - Read OCR
     *
     * 0th byte: R1 response
     *
     * 1st byte: OCR register [31:24]
     * IJKrrrrP (7th bit to 0th bit)
     * ||||||||
     * |||||||+-- 0th bit (P): Switching to 1.8V Accepted (S18A).
     * |||++++--- Reserved.
     * ||+------- 5th (K) bits: UHS-II Card Status.
     * |+-------- 6th (J) bits: Card Capacity Status (CCS).
     * +--------- 7th (I) bits: Card power up status bit (busy).
     *
     * 2nd byte: OCR register [23:16]
     * QRSTUVWX (7th bit to 0th bit)
     * ||||||||
     * |||||||+-- 0th bit (X): Voltage window 2.8-2.9V.
     * ||||||+--- 1th bit (W): Voltage window 2.9-3.0V.
     * |||||+---- 2th bit (V): Voltage window 3.0-3.1V.
     * ||||+----- 3th bit (U): Voltage window 3.1-3.2V.
     * |||+------ 4th bit (T): Voltage window 3.2-3.3V.
     * ||+------- 5th bit (S): Voltage window 3.3-3.4V.
     * |+-------- 6th bit (R): Voltage window 3.4-3.5V.
     * +--------- 7th bit (Q): Voltage window 3.5-3.6V.
     *
     * 3rd byte: OCR register [15:8]
     * Yrrrrrrr (7th bit to 0th bit)
     * ||||||||
     * |+++++++-- Reserved
     * +--------- 7th bit (Y): Voltage window 2.7-2.8V.
     *
     * 4th byte: OCR register [7:0]
     * grrrrrrr (7th bit to 0th bit)
     * ||||||||
     * |+++++++-- Reserved
     * +--------- 7th bit (g): Reserved for Low Voltage Range.
     */
    SD_send_cmd(58, 0x00000000, 0xFF);
    r = SD_read_r3(r3);
    SD_unselect_purge();
    if (r != HAL_OK)
      return r;
    if (r3[0] != 0)
      return HAL_ERROR;
    // another voltage check
    if (!(r3[2] & (0b00110000))) // 3.2-3.3V or 3.3-3.4V
      return HAL_ERROR;
    sd_high_capacity = (r3[1] & 0x40) >> 6;
  } else {
    return HAL_ERROR;
  }

  // set block length
  SD_send_cmd(16, SD_BLOCK_LENGTH, 0xFF);
  r = SD_read_r1(&r1);
  SD_unselect_purge();
  if (r != HAL_OK)
    return r;
  if (r1 != 0x00)
    return HAL_ERROR;

  return HAL_OK;
}

// multiple init tries
HAL_StatusTypeDef SD_init_tries()
{
  HAL_StatusTypeDef r;
  int i;

  for (i = 0; i < SD_INIT_TRIES; i++)
  {
    r = SD_init();
    if (r == HAL_OK)
      break;
  }
  return r;
}

// reduce SPI speed until SD card init is ok
HAL_StatusTypeDef SD_init_try_speed()
{
  HAL_StatusTypeDef r;

  SD_SPI_PORT.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  HAL_SPI_Init(&SD_SPI_PORT);
  r = SD_init_tries();
  if (r == HAL_OK)
    return r;
  SD_SPI_PORT.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  HAL_SPI_Init(&SD_SPI_PORT);
  r = SD_init_tries();
  if (r == HAL_OK)
    return r;
  SD_SPI_PORT.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
  HAL_SPI_Init(&SD_SPI_PORT);
  r = SD_init_tries();
  if (r == HAL_OK)
    return r;
  SD_SPI_PORT.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
  HAL_SPI_Init(&SD_SPI_PORT);
  return SD_init_tries();
}

HAL_StatusTypeDef SD_read_csd(SD_CSD* csd)
{
  HAL_StatusTypeDef r;
  uint8_t r1;
  uint8_t csd_data[16];

  // CMD9 - read CSD register and SD card capacity
  SD_send_cmd(9, 0x00000000, 0xFF);
  r = SD_read_r1(&r1);
  if (r != HAL_OK)
    return r;
  if (r1 != 0x00)
    return HAL_ERROR;
  r = SD_wait_data_token();
  if (r != HAL_OK)
    return r;
  SD_read_bytes(csd_data, sizeof(csd_data));

  SD_unselect_purge();

  csd->CSDStruct = (csd_data[0] & 0xC0) >> 6;
  csd->Reserved1 = csd_data[0] & 0x3F;
  csd->TAAC = csd_data[1];
  csd->NSAC = csd_data[2];
  csd->MaxBusClkFrec = csd_data[3];
  csd->CardComdClasses = (csd_data[4] << 4) | ((csd_data[5] & 0xF0) >> 4);
  csd->RdBlockLen = csd_data[5] & 0x0F;
  csd->PartBlockRead = (csd_data[6] & 0x80) >> 7;
  csd->WrBlockMisalign = (csd_data[6] & 0x40) >> 6;
  csd->RdBlockMisalign = (csd_data[6] & 0x20) >> 5;
  csd->DSRImpl = (csd_data[6] & 0x10) >> 4;
  switch (csd->CSDStruct) {
  case 0:
    // CSD version 1
    csd->version.v1.Reserved1 = ((csd_data[6] & 0x0C) >> 2);
    csd->version.v1.DeviceSize = ((csd_data[6] & 0x03) << 10) | (csd_data[7] << 2) |
                                 ((csd_data[8] & 0xC0) >> 6);
    csd->version.v1.MaxRdCurrentVDDMin = (csd_data[8] & 0x38) >> 3;
    csd->version.v1.MaxRdCurrentVDDMax = (csd_data[8] & 0x07);
    csd->version.v1.MaxWrCurrentVDDMin = (csd_data[9] & 0xE0) >> 5;
    csd->version.v1.MaxWrCurrentVDDMax = (csd_data[9] & 0x1C) >> 2;
    csd->version.v1.DeviceSizeMul = ((csd_data[9] & 0x03) << 1) |
                                    ((csd_data[10] & 0x80) >> 7);
    break;
  case 1:
    // CSD version 2
    csd->version.v2.Reserved1 = ((csd_data[6] & 0x0F) << 2) |
                                ((csd_data[7] & 0xC0) >> 6);
    csd->version.v2.DeviceSize = ((csd_data[7] & 0x3F) << 16) | (csd_data[8] << 8) |
                                 csd_data[9];
    csd->version.v2.Reserved2 = ((csd_data[10] & 0x80) >> 8);
    break;
  default:
    return HAL_ERROR;
  }
  csd->EraseSingleBlockEnable = (csd_data[10] & 0x40) >> 6;
  csd->EraseSectorSize = ((csd_data[10] & 0x3F) << 1) | ((csd_data[11] & 0x80) >> 7);
  csd->WrProtectGrSize = (csd_data[11] & 0x7F);
  csd->WrProtectGrEnable = (csd_data[12] & 0x80) >> 7;
  csd->Reserved2 = (csd_data[12] & 0x60) >> 5;
  csd->WrSpeedFact = (csd_data[12] & 0x1C) >> 2;
  csd->MaxWrBlockLen = ((csd_data[12] & 0x03) << 2) | ((csd_data[13] & 0xC0) >> 6);
  csd->WriteBlockPartial = (csd_data[13] & 0x20) >> 5;
  csd->Reserved3 = (csd_data[13] & 0x1F);
  csd->FileFormatGrouop = (csd_data[14] & 0x80) >> 7;
  csd->CopyFlag = (csd_data[14] & 0x40) >> 6;
  csd->PermWrProtect = (csd_data[14] & 0x20) >> 5;
  csd->TempWrProtect = (csd_data[14] & 0x10) >> 4;
  csd->FileFormat = (csd_data[14] & 0x0C) >> 2;
  csd->Reserved4 = (csd_data[14] & 0x03);
  csd->crc = (csd_data[15] & 0xFE) >> 1;
  csd->Reserved5 = (csd_data[15] & 0x01);

  return HAL_OK;
}

uint64_t SD_get_capacity()
{
  SD_CSD csd;
  HAL_StatusTypeDef r;
  r = SD_read_csd(&csd);
  if (r == HAL_OK)
  {
    if (sd_high_capacity)
      return (csd.version.v2.DeviceSize + 1) * SD_BLOCK_LENGTH * 1024;
    else
      return (csd.version.v1.DeviceSize + 1) * (1UL << (csd.version.v1.DeviceSizeMul + 2)) * (1UL << csd.RdBlockLen);
  }
  return 0;
}

HAL_StatusTypeDef SD_read_single_block(uint32_t blockNum, uint8_t *buff)
{
  HAL_StatusTypeDef r;
  uint8_t crc[2];
  uint8_t r1;

  if (!sd_high_capacity)
    blockNum *= SD_BLOCK_LENGTH;

  // CMD17 - send block
  SD_send_cmd(17, blockNum, 0xFF);
  r = SD_read_r1(&r1);
  if (r != HAL_OK)
    return r;
  if (r1 != 0x00)
    return HAL_ERROR;
  r = SD_wait_data_token();
  if (r != HAL_OK)
    return r;
  SD_read_bytes(buff, SD_BLOCK_LENGTH);
  SD_read_bytes(crc, 2);

  SD_unselect_purge();
  return HAL_OK;
}

HAL_StatusTypeDef SD_write_single_block(uint32_t blockNum, const uint8_t *buff)
{
  HAL_StatusTypeDef r;
  uint8_t r1;

  SD_select();

  if (!sd_high_capacity)
    blockNum *= SD_BLOCK_LENGTH;

  // CMD24 - write block
  SD_send_cmd(24, blockNum, 0xFF);
  r = SD_read_r1(&r1);
  if (r != HAL_OK)
    return r;
  if (r1 != 0x00)
    return HAL_ERROR;

  // send dummy bytes for NWR timing
  uint8_t dummy = 0xFF;
  SPI_transmit(&dummy, sizeof(dummy));
  SPI_transmit(&dummy, sizeof(dummy));

  // start token
  uint8_t dataToken = SD_DATA_TOKEN;
  uint8_t crc[2] = { 0xFF, 0xFF };
  SPI_transmit(&dataToken, sizeof(dataToken));
  SPI_transmit((uint8_t*)buff, SD_BLOCK_LENGTH);
  SPI_transmit(crc, sizeof(crc));

  /*
   dataResp:
   xxx0abc1
   010 - Data accepted
   101 - Data rejected due to CRC error
   110 - Data rejected due to write error
   */
  uint8_t dataResp;
  SD_read_bytes(&dataResp, sizeof(dataResp));
  if ((dataResp & 0x1F) != 0x05)
    return HAL_ERROR;

  r = SD_wait_not_busy();
  if (r != HAL_OK)
    return r;

  SD_unselect_purge();
  return HAL_OK;
}

HAL_StatusTypeDef SD_read_begin(uint32_t blockNum)
{
  HAL_StatusTypeDef r;
  uint8_t r1;

  SD_select();

  if (!sd_high_capacity)
    blockNum *= SD_BLOCK_LENGTH;

  /* CMD18 (READ_MULTIPLE_BLOCK) command */
  SD_send_cmd(18, blockNum, 0xFF);
  r = SD_read_r1(&r1);
  if (r != HAL_OK)
    return r;
  if (r1 != 0x00)
    return HAL_ERROR;

  SD_unselect_purge();
  return HAL_OK;
}

HAL_StatusTypeDef SD_read_data(uint8_t *buff)
{
  HAL_StatusTypeDef r;
  uint8_t crc[2];

  SD_select();

  r = SD_wait_data_token();
  if (r != HAL_OK)
    return r;
  SD_read_bytes(buff, SD_BLOCK_LENGTH);
  SD_read_bytes(crc, 2);

  SD_unselect_purge();
  return HAL_OK;
}

HAL_StatusTypeDef SD_read_end()
{
  HAL_StatusTypeDef r;
  uint8_t r1;

  SD_select();

  // CMD12 - stop transmission
  SD_send_cmd(12, 0x00000000, 0xFF);

  /*
   The received byte immediataly following CMD12 is a stuff byte, it should be
   discarded before receive the response of the CMD12
   */
  uint8_t stuffByte;
  SD_read_bytes(&stuffByte, sizeof(stuffByte));

  r = SD_read_r1(&r1);
  if (r != HAL_OK)
    return r;

  SD_unselect_purge();
  return HAL_OK;
}

HAL_StatusTypeDef SD_write_begin(uint32_t blockNum)
{
  HAL_StatusTypeDef r;
  uint8_t r1;

  SD_select();

  if (!sd_high_capacity)
    blockNum *= SD_BLOCK_LENGTH;

  // CMD25 - write multiple blocks
  SD_send_cmd(25, blockNum, 0xFF);
  r = SD_read_r1(&r1);
  if (r != HAL_OK)
    return r;
  if (r1 != 0x00)
    return HAL_ERROR;

  SD_unselect_purge();
  return HAL_OK;
}

HAL_StatusTypeDef SD_write_data(const uint8_t *buff)
{
  HAL_StatusTypeDef r;

  SD_select();

  uint8_t dataToken = SD_SEND_MULTIPLE_DATA_TOKEN;
  uint8_t crc[2] = { 0xFF, 0xFF };
  SPI_transmit(&dataToken, sizeof(dataToken));
  SPI_transmit((uint8_t*)buff, SD_BLOCK_LENGTH);
  SPI_transmit(crc, sizeof(crc));

  /*
   dataResp:
   xxx0abc1
   010 - Data accepted
   101 - Data rejected due to CRC error
   110 - Data rejected due to write error
   */
  uint8_t dataResp;
  SD_read_bytes(&dataResp, sizeof(dataResp));
  if ((dataResp & 0x1F) != 0x05)
    return HAL_ERROR;

  r = SD_wait_not_busy();
  if (r != HAL_OK)
    return r;

  SD_unselect_purge();
  return HAL_OK;
}

HAL_StatusTypeDef SD_write_end()
{
  HAL_StatusTypeDef r;

  SD_select();

  uint8_t stopTran = SD_STOP_DATA_TOKEN; // stop transaction token for CMD25
  SPI_transmit(&stopTran, sizeof(stopTran));

  // skip one byte before readyng "busy"
  // this is required by the spec and is necessary for some real SD-cards!
  uint8_t skipByte;
  SD_read_bytes(&skipByte, sizeof(skipByte));

  r = SD_wait_not_busy();
  if (r != HAL_OK)
    return r;

  SD_unselect_purge();
  return HAL_OK;
}
