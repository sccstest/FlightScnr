#pragma once

/**
 * Vendor init sequence for the Waveshare ESP32-P4-WIFI6-Touch-LCD-4C panel (720x720).
 * Copied from waveshareteam/ESP32-P4-WIFI6-Touch-LCD-XC (Apache-2.0),
 * examples/arduino/libraries/displays/displays_config.h, SCREEN_4INCH_DSI.
 */

#include "hardware/p4_dsi_panel.h"

namespace waveshare_p4_4c {

static const uint8_t kD0[] = {0x00};
static const uint8_t kD1[] = {0x93};
static const uint8_t kD2[] = {0x65};
static const uint8_t kD3[] = {0xF8};
static const uint8_t kD4[] = {0x01};
static const uint8_t kD5[] = {0x01};
static const uint8_t kD6[] = {0x00};
static const uint8_t kD7[] = {0x41};
static const uint8_t kD8[] = {0x10};
static const uint8_t kD9[] = {0x44};
static const uint8_t kD10[] = {0x00};
static const uint8_t kD11[] = {0xD0};
static const uint8_t kD12[] = {0x00};
static const uint8_t kD13[] = {0x00};
static const uint8_t kD14[] = {0xD0};
static const uint8_t kD15[] = {0x00};
static const uint8_t kD16[] = {0xFE};
static const uint8_t kD17[] = {0x26};
static const uint8_t kD18[] = {0x09};
static const uint8_t kD19[] = {0x04};
static const uint8_t kD20[] = {0x08};
static const uint8_t kD21[] = {0x0A};
static const uint8_t kD22[] = {0x78};
static const uint8_t kD23[] = {0xFF};
static const uint8_t kD24[] = {0xFF};
static const uint8_t kD25[] = {0xFF};
static const uint8_t kD26[] = {0x04};
static const uint8_t kD27[] = {0x64};
static const uint8_t kD28[] = {0xC7};
static const uint8_t kD29[] = {0x18};
static const uint8_t kD30[] = {0x0B};
static const uint8_t kD31[] = {0x14};
static const uint8_t kD32[] = {0x02};
static const uint8_t kD33[] = {0x49};
static const uint8_t kD34[] = {0x0A};
static const uint8_t kD35[] = {0x1B};
static const uint8_t kD36[] = {0x19};
static const uint8_t kD37[] = {0x7F};
static const uint8_t kD38[] = {0x56};
static const uint8_t kD39[] = {0x43};
static const uint8_t kD40[] = {0x37};
static const uint8_t kD41[] = {0x33};
static const uint8_t kD42[] = {0x25};
static const uint8_t kD43[] = {0x2A};
static const uint8_t kD44[] = {0x16};
static const uint8_t kD45[] = {0x30};
static const uint8_t kD46[] = {0x2F};
static const uint8_t kD47[] = {0x32};
static const uint8_t kD48[] = {0x53};
static const uint8_t kD49[] = {0x43};
static const uint8_t kD50[] = {0x4C};
static const uint8_t kD51[] = {0x40};
static const uint8_t kD52[] = {0x3D};
static const uint8_t kD53[] = {0x31};
static const uint8_t kD54[] = {0x20};
static const uint8_t kD55[] = {0x0F};
static const uint8_t kD56[] = {0x7F};
static const uint8_t kD57[] = {0x56};
static const uint8_t kD58[] = {0x43};
static const uint8_t kD59[] = {0x37};
static const uint8_t kD60[] = {0x33};
static const uint8_t kD61[] = {0x25};
static const uint8_t kD62[] = {0x2A};
static const uint8_t kD63[] = {0x16};
static const uint8_t kD64[] = {0x30};
static const uint8_t kD65[] = {0x2F};
static const uint8_t kD66[] = {0x32};
static const uint8_t kD67[] = {0x53};
static const uint8_t kD68[] = {0x43};
static const uint8_t kD69[] = {0x4C};
static const uint8_t kD70[] = {0x40};
static const uint8_t kD71[] = {0x3D};
static const uint8_t kD72[] = {0x31};
static const uint8_t kD73[] = {0x20};
static const uint8_t kD74[] = {0x0F};
static const uint8_t kD75[] = {0x02};
static const uint8_t kD76[] = {0x5F};
static const uint8_t kD77[] = {0x5F};
static const uint8_t kD78[] = {0x5E};
static const uint8_t kD79[] = {0x5E};
static const uint8_t kD80[] = {0x50};
static const uint8_t kD81[] = {0x48};
static const uint8_t kD82[] = {0x48};
static const uint8_t kD83[] = {0x4A};
static const uint8_t kD84[] = {0x4A};
static const uint8_t kD85[] = {0x44};
static const uint8_t kD86[] = {0x44};
static const uint8_t kD87[] = {0x46};
static const uint8_t kD88[] = {0x46};
static const uint8_t kD89[] = {0x5F};
static const uint8_t kD90[] = {0x5F};
static const uint8_t kD91[] = {0x57};
static const uint8_t kD92[] = {0x57};
static const uint8_t kD93[] = {0x77};
static const uint8_t kD94[] = {0x77};
static const uint8_t kD95[] = {0x40};
static const uint8_t kD96[] = {0x42};
static const uint8_t kD97[] = {0x5F};
static const uint8_t kD98[] = {0x5F};
static const uint8_t kD99[] = {0x5F};
static const uint8_t kD100[] = {0x5E};
static const uint8_t kD101[] = {0x5E};
static const uint8_t kD102[] = {0x50};
static const uint8_t kD103[] = {0x49};
static const uint8_t kD104[] = {0x49};
static const uint8_t kD105[] = {0x4B};
static const uint8_t kD106[] = {0x4B};
static const uint8_t kD107[] = {0x45};
static const uint8_t kD108[] = {0x45};
static const uint8_t kD109[] = {0x47};
static const uint8_t kD110[] = {0x47};
static const uint8_t kD111[] = {0x5F};
static const uint8_t kD112[] = {0x5F};
static const uint8_t kD113[] = {0x57};
static const uint8_t kD114[] = {0x57};
static const uint8_t kD115[] = {0x77};
static const uint8_t kD116[] = {0x77};
static const uint8_t kD117[] = {0x41};
static const uint8_t kD118[] = {0x43};
static const uint8_t kD119[] = {0x5F};
static const uint8_t kD120[] = {0x1E};
static const uint8_t kD121[] = {0x1E};
static const uint8_t kD122[] = {0x1F};
static const uint8_t kD123[] = {0x1F};
static const uint8_t kD124[] = {0x10};
static const uint8_t kD125[] = {0x07};
static const uint8_t kD126[] = {0x07};
static const uint8_t kD127[] = {0x05};
static const uint8_t kD128[] = {0x05};
static const uint8_t kD129[] = {0x0B};
static const uint8_t kD130[] = {0x0B};
static const uint8_t kD131[] = {0x09};
static const uint8_t kD132[] = {0x09};
static const uint8_t kD133[] = {0x1F};
static const uint8_t kD134[] = {0x1F};
static const uint8_t kD135[] = {0x17};
static const uint8_t kD136[] = {0x17};
static const uint8_t kD137[] = {0x17};
static const uint8_t kD138[] = {0x17};
static const uint8_t kD139[] = {0x03};
static const uint8_t kD140[] = {0x01};
static const uint8_t kD141[] = {0x1F};
static const uint8_t kD142[] = {0x1E};
static const uint8_t kD143[] = {0x1E};
static const uint8_t kD144[] = {0x1F};
static const uint8_t kD145[] = {0x1F};
static const uint8_t kD146[] = {0x10};
static const uint8_t kD147[] = {0x06};
static const uint8_t kD148[] = {0x06};
static const uint8_t kD149[] = {0x04};
static const uint8_t kD150[] = {0x04};
static const uint8_t kD151[] = {0x0A};
static const uint8_t kD152[] = {0x0A};
static const uint8_t kD153[] = {0x08};
static const uint8_t kD154[] = {0x08};
static const uint8_t kD155[] = {0x1F};
static const uint8_t kD156[] = {0x1F};
static const uint8_t kD157[] = {0x17};
static const uint8_t kD158[] = {0x17};
static const uint8_t kD159[] = {0x17};
static const uint8_t kD160[] = {0x17};
static const uint8_t kD161[] = {0x02};
static const uint8_t kD162[] = {0x00};
static const uint8_t kD163[] = {0x1F};
static const uint8_t kD164[] = {0x02};
static const uint8_t kD165[] = {0x40};
static const uint8_t kD166[] = {0x00};
static const uint8_t kD167[] = {0x00};
static const uint8_t kD168[] = {0x30};
static const uint8_t kD169[] = {0x01};
static const uint8_t kD170[] = {0x30};
static const uint8_t kD171[] = {0x01};
static const uint8_t kD172[] = {0x02};
static const uint8_t kD173[] = {0x30};
static const uint8_t kD174[] = {0x03};
static const uint8_t kD175[] = {0x04};
static const uint8_t kD176[] = {0x04};
static const uint8_t kD177[] = {0xA6};
static const uint8_t kD178[] = {0x43};
static const uint8_t kD179[] = {0x30};
static const uint8_t kD180[] = {0x73};
static const uint8_t kD181[] = {0x05};
static const uint8_t kD182[] = {0x04};
static const uint8_t kD183[] = {0x7F};
static const uint8_t kD184[] = {0x08};
static const uint8_t kD185[] = {0x00};
static const uint8_t kD186[] = {0x04};
static const uint8_t kD187[] = {0x04};
static const uint8_t kD188[] = {0x88};
static const uint8_t kD189[] = {0xD9};
static const uint8_t kD190[] = {0x00};
static const uint8_t kD191[] = {0x33};
static const uint8_t kD192[] = {0x43};
static const uint8_t kD193[] = {0x00};
static const uint8_t kD194[] = {0x00};
static const uint8_t kD195[] = {0x00};
static const uint8_t kD196[] = {0x00};

static const P4DsiInitCmd kInit[] = {
    {0xE0, kD0, 1, 0},
    {0xE1, kD1, 1, 0},
    {0xE2, kD2, 1, 0},
    {0xE3, kD3, 1, 0},
    {0x80, kD4, 1, 0},
    {0xE0, kD5, 1, 0},
    {0x00, kD6, 1, 0},
    {0x01, kD7, 1, 0},
    {0x03, kD8, 1, 0},
    {0x04, kD9, 1, 0},
    {0x17, kD10, 1, 0},
    {0x18, kD11, 1, 0},
    {0x19, kD12, 1, 0},
    {0x1A, kD13, 1, 0},
    {0x1B, kD14, 1, 0},
    {0x1C, kD15, 1, 0},
    {0x24, kD16, 1, 0},
    {0x35, kD17, 1, 0},
    {0x37, kD18, 1, 0},
    {0x38, kD19, 1, 0},
    {0x39, kD20, 1, 0},
    {0x3A, kD21, 1, 0},
    {0x3C, kD22, 1, 0},
    {0x3D, kD23, 1, 0},
    {0x3E, kD24, 1, 0},
    {0x3F, kD25, 1, 0},
    {0x40, kD26, 1, 0},
    {0x41, kD27, 1, 0},
    {0x42, kD28, 1, 0},
    {0x43, kD29, 1, 0},
    {0x44, kD30, 1, 0},
    {0x45, kD31, 1, 0},
    {0x55, kD32, 1, 0},
    {0x57, kD33, 1, 0},
    {0x59, kD34, 1, 0},
    {0x5A, kD35, 1, 0},
    {0x5B, kD36, 1, 0},
    {0x5D, kD37, 1, 0},
    {0x5E, kD38, 1, 0},
    {0x5F, kD39, 1, 0},
    {0x60, kD40, 1, 0},
    {0x61, kD41, 1, 0},
    {0x62, kD42, 1, 0},
    {0x63, kD43, 1, 0},
    {0x64, kD44, 1, 0},
    {0x65, kD45, 1, 0},
    {0x66, kD46, 1, 0},
    {0x67, kD47, 1, 0},
    {0x68, kD48, 1, 0},
    {0x69, kD49, 1, 0},
    {0x6A, kD50, 1, 0},
    {0x6B, kD51, 1, 0},
    {0x6C, kD52, 1, 0},
    {0x6D, kD53, 1, 0},
    {0x6E, kD54, 1, 0},
    {0x6F, kD55, 1, 0},
    {0x70, kD56, 1, 0},
    {0x71, kD57, 1, 0},
    {0x72, kD58, 1, 0},
    {0x73, kD59, 1, 0},
    {0x74, kD60, 1, 0},
    {0x75, kD61, 1, 0},
    {0x76, kD62, 1, 0},
    {0x77, kD63, 1, 0},
    {0x78, kD64, 1, 0},
    {0x79, kD65, 1, 0},
    {0x7A, kD66, 1, 0},
    {0x7B, kD67, 1, 0},
    {0x7C, kD68, 1, 0},
    {0x7D, kD69, 1, 0},
    {0x7E, kD70, 1, 0},
    {0x7F, kD71, 1, 0},
    {0x80, kD72, 1, 0},
    {0x81, kD73, 1, 0},
    {0x82, kD74, 1, 0},
    {0xE0, kD75, 1, 0},
    {0x00, kD76, 1, 0},
    {0x01, kD77, 1, 0},
    {0x02, kD78, 1, 0},
    {0x03, kD79, 1, 0},
    {0x04, kD80, 1, 0},
    {0x05, kD81, 1, 0},
    {0x06, kD82, 1, 0},
    {0x07, kD83, 1, 0},
    {0x08, kD84, 1, 0},
    {0x09, kD85, 1, 0},
    {0x0A, kD86, 1, 0},
    {0x0B, kD87, 1, 0},
    {0x0C, kD88, 1, 0},
    {0x0D, kD89, 1, 0},
    {0x0E, kD90, 1, 0},
    {0x0F, kD91, 1, 0},
    {0x10, kD92, 1, 0},
    {0x11, kD93, 1, 0},
    {0x12, kD94, 1, 0},
    {0x13, kD95, 1, 0},
    {0x14, kD96, 1, 0},
    {0x15, kD97, 1, 0},
    {0x16, kD98, 1, 0},
    {0x17, kD99, 1, 0},
    {0x18, kD100, 1, 0},
    {0x19, kD101, 1, 0},
    {0x1A, kD102, 1, 0},
    {0x1B, kD103, 1, 0},
    {0x1C, kD104, 1, 0},
    {0x1D, kD105, 1, 0},
    {0x1E, kD106, 1, 0},
    {0x1F, kD107, 1, 0},
    {0x20, kD108, 1, 0},
    {0x21, kD109, 1, 0},
    {0x22, kD110, 1, 0},
    {0x23, kD111, 1, 0},
    {0x24, kD112, 1, 0},
    {0x25, kD113, 1, 0},
    {0x26, kD114, 1, 0},
    {0x27, kD115, 1, 0},
    {0x28, kD116, 1, 0},
    {0x29, kD117, 1, 0},
    {0x2A, kD118, 1, 0},
    {0x2B, kD119, 1, 0},
    {0x2C, kD120, 1, 0},
    {0x2D, kD121, 1, 0},
    {0x2E, kD122, 1, 0},
    {0x2F, kD123, 1, 0},
    {0x30, kD124, 1, 0},
    {0x31, kD125, 1, 0},
    {0x32, kD126, 1, 0},
    {0x33, kD127, 1, 0},
    {0x34, kD128, 1, 0},
    {0x35, kD129, 1, 0},
    {0x36, kD130, 1, 0},
    {0x37, kD131, 1, 0},
    {0x38, kD132, 1, 0},
    {0x39, kD133, 1, 0},
    {0x3A, kD134, 1, 0},
    {0x3B, kD135, 1, 0},
    {0x3C, kD136, 1, 0},
    {0x3D, kD137, 1, 0},
    {0x3E, kD138, 1, 0},
    {0x3F, kD139, 1, 0},
    {0x40, kD140, 1, 0},
    {0x41, kD141, 1, 0},
    {0x42, kD142, 1, 0},
    {0x43, kD143, 1, 0},
    {0x44, kD144, 1, 0},
    {0x45, kD145, 1, 0},
    {0x46, kD146, 1, 0},
    {0x47, kD147, 1, 0},
    {0x48, kD148, 1, 0},
    {0x49, kD149, 1, 0},
    {0x4A, kD150, 1, 0},
    {0x4B, kD151, 1, 0},
    {0x4C, kD152, 1, 0},
    {0x4D, kD153, 1, 0},
    {0x4E, kD154, 1, 0},
    {0x4F, kD155, 1, 0},
    {0x50, kD156, 1, 0},
    {0x51, kD157, 1, 0},
    {0x52, kD158, 1, 0},
    {0x53, kD159, 1, 0},
    {0x54, kD160, 1, 0},
    {0x55, kD161, 1, 0},
    {0x56, kD162, 1, 0},
    {0x57, kD163, 1, 0},
    {0xE0, kD164, 1, 0},
    {0x58, kD165, 1, 0},
    {0x59, kD166, 1, 0},
    {0x5A, kD167, 1, 0},
    {0x5B, kD168, 1, 0},
    {0x5C, kD169, 1, 0},
    {0x5D, kD170, 1, 0},
    {0x5E, kD171, 1, 0},
    {0x5F, kD172, 1, 0},
    {0x60, kD173, 1, 0},
    {0x61, kD174, 1, 0},
    {0x62, kD175, 1, 0},
    {0x63, kD176, 1, 0},
    {0x64, kD177, 1, 0},
    {0x65, kD178, 1, 0},
    {0x66, kD179, 1, 0},
    {0x67, kD180, 1, 0},
    {0x68, kD181, 1, 0},
    {0x69, kD182, 1, 0},
    {0x6A, kD183, 1, 0},
    {0x6B, kD184, 1, 0},
    {0x6C, kD185, 1, 0},
    {0x6D, kD186, 1, 0},
    {0x6E, kD187, 1, 0},
    {0x6F, kD188, 1, 0},
    {0x75, kD189, 1, 0},
    {0x76, kD190, 1, 0},
    {0x77, kD191, 1, 0},
    {0x78, kD192, 1, 0},
    {0xE0, kD193, 1, 0},
    {0x11, kD194, 1, 120},
    {0x29, kD195, 1, 20},
    {0x35, kD196, 1, 0},
};

}  // namespace waveshare_p4_4c
