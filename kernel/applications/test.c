#include "sys.h"
#include "stdio.h"
#include "string.h"

int main() {
    printf("Hello World!\n");
    
    char name[50] = "John";
    printf("My name is %s\n", name);
    
    char greeting[100];
    strcpy(greeting, "Hello, ");
    strcat(greeting, name);
    printf("%s\n", greeting);
    
    char text[] = "This is a test";
    printf("Text: %s (length: %d)\n", text, strlen(text));
    
    int age = 25;
    printf("I am %d years old\n", age);
    
    char buffer[50];
    sprintf(buffer, "Age: %d", age);
    printf("%s\n", buffer);
    
    printf("Done!\n");
    
    return 0;
}