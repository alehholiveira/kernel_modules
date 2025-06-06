# kernel_modules
process
   - make
   - sudo insmod process_risk.ko
   - ls /proc/process_risk/
   - cat /proc/process_risk
   - sudo rmmod process_risk.ko
   - make clean