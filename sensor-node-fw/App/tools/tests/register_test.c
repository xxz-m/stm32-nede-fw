/* 主机测试：实际寄存器服务、地址映射及云端分发，Store和基础命令副作用用替身计数。 */
#include "cloud_command.h"
#include "store.h"
#include "svc_frame.h"
#include "modem_command.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* 直接测试实际私有下行入口，不为测试扩大组件API。 */
#include "../../component/modem/modem_component.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static unsigned writes, basics, checks;
static bool resolved = true, present = true;
static svc_status_t writeStatus = SVC_OK;
static char period[8] = "20", sn[24] = "NODE0001";
static char uuid[40] = "CDAB0000-1234-5678-1234-56789ABCDEF0";
char const *App_Store_Value(uint8_t id) {
    return id == SVC_PARAM_TRANSINTVL ? period : id == SVC_PARAM_BLE_SN ? sn : uuid;
}
bool App_Store_ConfigResolved(void) { return resolved; }
void App_Store_Info(AppStoreInfo *out) { memset(out,0,sizeof(*out)); out->resolved=resolved; out->present=present; }
svc_status_t App_Store_SetName(char const *name, char const *value) {
    CHECK(strcmp(name,"transintvl")==0);
    if(writeStatus!=SVC_OK) { return writeStatus; }
    strcpy(period,value); ++writes; return SVC_OK;
}
bool App_Command_Dispatch(AppCommandRequest const *request, AppCommandResult *result) {
    CHECK(request->command.kind==SVC_AT_KIND_PING || request->command.kind==SVC_AT_KIND_REBOOT);
    ++basics; result->status=APP_COMMAND_RESULT_DONE; strcpy(result->raw,"OK\r\n"); return true;
}
/** @brief 参考CRC按位实现独立于生产例程，并以手册固定向量作锚点。 */
static void frame(char *out, char const *body) {
    unsigned crc=0xFFFFU, bit; char const *p=body;
    while(*p) { crc^=(unsigned char)*p++; for(bit=0;bit<8;++bit) { crc=(crc>>1)^((crc&1U)?0xA001U:0U); } }
    sprintf(out,"%s,%02X%02X",body,crc&255U,crc>>8);
}
static AppRegisterSnapshot sample = {{2,-123,0},{-1000,1000,-1},2509,3700,true,true};
/** @brief 自动补请求CRC，但期望应答主体由测试显式给定。 */
static void exchange(bool write, char const *body, char const *expected) {
    AppCommandRequest req={0}; AppCommandResult out; char want[192];
    req.source=APP_COMMAND_SOURCE_MODEM;
    req.command.kind=write?SVC_AT_KIND_REGISTER_WRITE:SVC_AT_KIND_REGISTER_READ;
    frame(req.command.value,body); frame(want,expected);
    CHECK(App_Register_Dispatch(&req,&sample,&out)); CHECK(strcmp(out.raw,want)==0); ++checks;
}
static void testMap(void) {
    exchange(true,"01,0005","01,00"); CHECK(strcmp(period,"5")==0);
    exchange(false,"01,02,03,20","01,0005,02,0003,03,000F,20,0003");
    exchange(false,"10,11,12,13,14,15,16,17","10,0002,11,FF85,12,0000,13,09CD,14,0E74,15,FC18,16,03E8,17,FFFF");
    exchange(false,"10,FF,10","10,0002,FF,XY,10,0002");
    sample.valid=false; exchange(false,"10,14,FF","10,XX,14,0E74,FF,XY"); sample.valid=true;
    sample.battery_valid=false; exchange(false,"14,12","14,XX,12,0000"); sample.battery_valid=true;
    sample.angle_cdeg[0]=65535; exchange(false,"10","10,XX"); sample.angle_cdeg[0]=2;
    exchange(true,"01,0000,01,0015,02,0005,03,0000,03,0010","01,OV,01,OV,02,OV,03,OV,03,OV");
    exchange(true,"01,0001,01,0014,10,0001,FF,0001,02,0004,03,0001","01,00,01,00,10,RO,FF,XY,02,NP,03,NP");
    CHECK(strcmp(period,"20")==0);
    exchange(true,"01,00a5,01,00G5","01,IC,01,IC");
    present=false; exchange(true,"01,0005","01,SE"); present=true;
    resolved=false; exchange(true,"01,0005","01,SE"); exchange(false,"01,40,50","01,XX,40,XX,50,XX"); resolved=true;
    writeStatus=SVC_ERR_STATE; exchange(true,"01,0005","01,SE"); writeStatus=SVC_OK;
    exchange(false,"40,41,42,43,44,4B","40,4E4F,41,4445,42,3030,43,3031,44,0000,4B,0000");
    exchange(false,"50,51,52,53,54,55","50,3536,51,3738,52,3941,53,4243,54,4445,55,4630");
    strcpy(sn,"12345678901234567890123"); exchange(false,"4B","4B,3300"); strcpy(sn,"NODE0001");
    uuid[0]='E'; exchange(false,"50","50,XX"); uuid[0]='C';
    exchange(true,"14,0000,20,0000,40,0000,55,0000","14,RO,20,RO,40,RO,55,RO");
    puts("PASS address map, signed measurements, validity, identity, range and partial writes");
}
static void testFrames(void) {
    AppCommandRequest req={0}; AppCommandResult out; char payload[200], body[180]=""; unsigned before=writes,i;
    char const *bad[]={"01,0005,0000","01,0005,65fc","01,05,0000","1,0005,0000","01,0005,FF,0001,,0000"};
    req.source=APP_COMMAND_SOURCE_MODEM; req.command.kind=SVC_AT_KIND_REGISTER_WRITE;
    for(i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        strcpy(req.command.value,bad[i]); CHECK(App_Register_Dispatch(&req,&sample,&out));
        CHECK(strncmp(out.raw,"+CME ERROR:",11)==0);
    }
    CHECK(writes==before); CHECK(out.error_code==2U);
    strcpy(req.command.value,"01,0005,0000"); App_Register_Dispatch(&req,&sample,&out); CHECK(out.error_code==6U);
    for(i=0;i<16;++i) { strcat(body,i?",10":"10"); } frame(req.command.value,body);
    req.command.kind=SVC_AT_KIND_REGISTER_READ; CHECK(strlen(req.command.value)==52);
    App_Register_Dispatch(&req,&sample,&out); CHECK(strlen(out.raw)==132);
    strcat(body,",10"); frame(payload,body); strcpy(req.command.value,payload);
    App_Register_Dispatch(&req,&sample,&out); CHECK(out.error_code==2U);
    req.source=APP_COMMAND_SOURCE_SERIAL; strcpy(req.command.value,"10,1434");
    App_Register_Dispatch(&req,&sample,&out); frame(payload,"10,0002"); sprintf(body,"+READ:\"%s\"\r\nOK\r\n",payload);
    CHECK(strcmp(out.raw,body)==0);
    { SvcAt at; SvcAtCmd cmd; char const *p="AT+WRITE=\"01,0005,65FC\"\r";
      SVC_At_Init(&at); while(*p) { bool done=SVC_At_Feed(&at,(uint8_t)*p++,&cmd); if(!*p) { CHECK(done); } }
      CHECK(cmd.kind==SVC_AT_KIND_REGISTER_WRITE && strcmp(cmd.value,"01,0005,65FC")==0); }
    puts("PASS CRC fixed vectors, frame rejection without writes, full-length and serial adapter");
}
static void testJson(void) {
    struct ModemCommandRequest wire; char out[384], json[384], id[65]; unsigned i;
    char const *bad[]={"{\"id\":\"x\",\"command\":\"AT\",\"command\":\"AT+RST\"}",
      "{\"id\":\"x\",\"command\":\"AT\"}junk","{\"id\":\"x\",\"command\":\"AT\\nAT+RST\"}",
      "{\"id\":\"x\",\"command\":\"AT\",}","{\"id\":1,\"command\":\"AT\"}",
      "{\"id\":\"x\",\"command\":\"AT\\u0000+RST\"}","{\"id\":\"x\",\"command\":\"AT\",\"extra\":\"x\"}"};
    for(i=0;i<sizeof(bad)/sizeof(bad[0]);++i) { CHECK(!Modem_Command_Parse(bad[i],&wire)); }
    CHECK(Modem_Command_Parse(" { \"data\":\"10,1434\",\"command\":\"AT+READ\",\"id\":\"x\\u0031\" } ",&wire));
    CHECK(strcmp(wire.id,"x1")==0 && wire.has_data);
    CHECK(Modem_Command_Format(out,sizeof(out),"x","AT","OK\r\n"));
    CHECK(strcmp(out,"{\"id\":\"x\",\"command\":\"AT\",\"data\":\"OK\\r\\n\"}")==0);
    CHECK(Modem_Command_Format(out,sizeof(out),"x","AT+WRITE","01,00,BF25"));
    CHECK(strcmp(out,"{\"id\":\"x\",\"data\":\"01,00,BF25\"}")==0);
    CHECK(Modem_Command_Format(out,sizeof(out),"x","AT+WRITE","ERROR\r\n"));
    CHECK(strcmp(out,"{\"id\":\"x\",\"data\":\"ERROR\"}")==0);
    CHECK(!Modem_Command_Format(out,8,"x","AT","OK\r\n") && out[0]==0);
    strcpy(json,"{\"id\":\"x\",\"command\":\"AT+WRITE\",\"data\":\"01,0005,65FC\"}");
    { size_t len=strlen(json),cut; for(cut=0;cut<len;++cut) {
        char saved=json[cut]; json[cut]=0; CHECK(!Modem_Command_Parse(json,&wire)); json[cut]=saved;
      } }
    memset(id,'A',63); id[63]=0; sprintf(json,"{\"id\":\"%s\",\"command\":\"AT\"}",id);
    CHECK(Modem_Command_Parse(json,&wire)); id[63]='A'; id[64]=0;
    sprintf(json,"{\"id\":\"%s\",\"command\":\"AT\"}",id); CHECK(!Modem_Command_Parse(json,&wire));
    puts("PASS strict JSON, escaped ASCII, duplicate/control rejection, limits and exact response shape");
}
static void testCloud(void) {
    ModemComponent m={0}; struct ModemCommandRequest wire; AppCommandResult result;
    unsigned before=writes, first=basics; char saved[384];
    strcpy(m.payload,"{\"id\":\"w\",\"command\":\"AT+WRITE\",\"data\":\"01,0005,65FC\"}");
    dispatchDownlink(&m,MODEM_TOPIC_COMMAND); CHECK(m.downlink_pending);
    strcpy(m.payload,"{\"id\":\"second\",\"command\":\"AT+RST\"}");
    dispatchDownlink(&m,MODEM_TOPIC_COMMAND); CHECK(strcmp(m.downlink.id,"w")==0);
    CHECK(Modem_Component_TakeCommand(&m,&wire)); App_CloudCommand_Dispatch(&wire,&sample,&result);
    Modem_Component_CompleteCommand(&m,wire.id,wire.command,result.raw);
    CHECK(writes==before+1 && strcmp(m.reply_payload,"{\"id\":\"w\",\"data\":\"01,00,BF25\"}")==0);
    strcpy(saved,m.reply_payload); dispatchDownlink(&m,MODEM_TOPIC_COMMAND);
    CHECK(!m.downlink_pending && strcmp(saved,m.reply_payload)==0 && basics==first);
    strcpy(m.payload,"{\"cmd\":\"ping\"}"); dispatchDownlink(&m,MODEM_TOPIC_STATE);
    CHECK(strcmp(saved,m.reply_payload)==0);
    strcpy(m.payload,"{\"id\":\"second\",\"command\":\"AT+RST\"}");
    m.reply_pending=false; m.runtime_op=MODEM_RUNTIME_REPLY; dispatchDownlink(&m,MODEM_TOPIC_COMMAND); CHECK(!m.downlink_pending);
    m.runtime_op=MODEM_RUNTIME_NONE; dispatchDownlink(&m,MODEM_TOPIC_COMMAND); CHECK(Modem_Component_TakeCommand(&m,&wire));
    App_CloudCommand_Dispatch(&wire,&sample,&result); CHECK(basics==first+1 && strcmp(result.raw,"OK\r\n")==0);
    strcpy(wire.command,"AT+SN=\"changed\""); App_CloudCommand_Dispatch(&wire,&sample,&result); CHECK(strcmp(result.raw,"ERROR\r\n")==0);
    strcpy(wire.command,"AT+ZERO=\"CONFIRM\""); App_CloudCommand_Dispatch(&wire,&sample,&result); CHECK(strcmp(result.raw,"ERROR\r\n")==0);
    strcpy(wire.command,"AT+RST"); wire.has_data=true; App_CloudCommand_Dispatch(&wire,&sample,&result); CHECK(basics==first+1);
    strcpy(wire.command,"AT+WRITE"); strcpy(wire.data,"01,0005,0000");
    App_CloudCommand_Dispatch(&wire,&sample,&result);
    CHECK(strcmp(result.raw,"+CME ERROR:6")==0 && writes==before+1);
    wire.data[0]=0; wire.has_data=false; App_CloudCommand_Dispatch(&wire,&sample,&result);
    CHECK(strcmp(result.raw,"+CME ERROR:2")==0 && writes==before+1);
    puts("PASS actual cloud mailbox, App execution, reply retention and command whitelist");
}
int main(void) {
    char out[64]; frame(out,"01,0005"); CHECK(strcmp(out,"01,0005,65FC")==0);
    frame(out,"01,00"); CHECK(strcmp(out,"01,00,BF25")==0);
    testMap(); testFrames(); testJson(); testCloud();
    printf("PASS register suite: 4 groups, %u mapped exchanges\n",checks); return 0;
}
