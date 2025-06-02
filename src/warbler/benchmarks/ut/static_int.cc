#include <stdio.h> 
#include <iostream>
#include <vector>

using namespace std;
namespace ns { 
  static int a; 
} 
 
 
void test_ns() 
{ 
  ns::a = 7; 
  printf("ns::a = %d\n",ns::a); 
  ns::a ++; 
  printf("after incrementation ns::a = %d\n",ns::a); 
} 
 
// this could be in another file, in which case use a header file 
// to declare void test_ns(): 
int main() 
{ 

std::vector<double>  workload;
workload.push_back(0.9);
workload.push_back(0.1);
int a=0, b=0;
for (int i=0; i<100;i++){
    double d = rand() / double(RAND_MAX);
    for (size_t i = 0; i < workload.size(); i++) {
      if ((i + 1) == workload.size() || d < workload[i]) {
        std::cout << workload[i] << std::endl;
        if (i==0)a++;
        else b++;
        break;
      }
      d -= workload[i];
    }
}
std::cout<<"a="<<a<<",b="<<b<<std::endl;
return 0; 
} 