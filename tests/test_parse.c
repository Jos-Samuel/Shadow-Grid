#include <stdio.h>
#include <string.h>

int main() {
    const char *buffer = "TYPE:ROLE;SET:ADMIN;PASS:secret123";
    char dir[16] = {0};
    char pass[32] = {0};
    
    const char *role_ptr = strstr(buffer, "SET:");
    if (role_ptr) sscanf(role_ptr, "SET:%15[^;]", dir);
    const char *pass_ptr = strstr(buffer, "PASS:");
    if (pass_ptr) sscanf(pass_ptr, "PASS:%31[^;]", pass);
    
    printf("DIR: '%s'\n", dir);
    printf("PASS: '%s'\n", pass);
    return 0;
}
