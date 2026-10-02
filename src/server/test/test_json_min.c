/* Regression test for the JSON helpers. Built and run by `make test`. */
#include <stdio.h>
#include <string.h>
#include "json_min.h"
int main(void){
  char buf[512]; int fails=0;
  #define CHECK(name,cond) do{ if(cond){printf("  [PASS] %s\n",name);} else {printf("  [FAIL] %s\n",name);fails++;} }while(0)
  const char *j = "{\"model\":\"gyro\",\"text\":\"سلام دنیا\",\"speed\":1.25,\"sid\":0,\"ok\":true}";
  CHECK("plain string", json_get_string(j,"model",buf,sizeof buf) && !strcmp(buf,"gyro"));
  CHECK("utf8 passthrough", json_get_string(j,"text",buf,sizeof buf) && strstr(buf,"سلام")!=NULL);
  double d; CHECK("double", json_get_double(j,"speed",&d) && d>1.24 && d<1.26);
  int i; CHECK("int", json_get_int(j,"sid",&i) && i==0);
  CHECK("missing key", json_get_string(j,"nope",buf,sizeof buf)==0);
  const char *e = "{\"text\":\"a\\nb\\tc\\\\d\\\"e\\u00e9\\uD83D\\uDE00\"}";
  CHECK("escapes", json_get_string(e,"text",buf,sizeof buf));
  printf("         decoded: "); 
  { int k; for(k=0;buf[k];k++) printf("%02X ", (unsigned char)buf[k]); printf("\n"); }
  char out[256]; json_escape("a\"b\\c\nd", out, sizeof out);
  CHECK("escape out", !strcmp(out,"a\\\"b\\\\c\\nd"));
  /* a \u escape must not loop forever */
  CHECK("truncated escape terminates", json_get_string("{\"x\":\"\\u00\"}","x",buf,sizeof buf)==0 || buf[0]);
  printf("RESULT: %s\n", fails?"FAILURES":"ALL PASS");
  return fails?1:0;
}
