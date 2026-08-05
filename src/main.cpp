/**
 * @file main.cpp
 * @author Ernest
 * @brief 
 * @date 2026-08-06
 */

#include<iostream>
#include<memory>
#include <functional>
#include<thread>
#include"Ccurl.h"

using namespace std;


/**
 * @brief 程序入口
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 程序退出码
 */
int main(int argc, char **argv)
{
  if(argc > 1)
  {

  }

  unique_ptr<Ccurl> ptr = make_unique<Ccurl>();

  ptr->Init("https://releases.ubuntu.com/20.04/ubuntu-20.04.6-live-server-amd64.iso.zsync", "./test");
  
  ptr->Download_Task();
  
  return 0;
}
