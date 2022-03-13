Introduction
------------

srandom is a Linux kernel module that can be used to replace the built-in /dev/urandom & /dev/random device files.  It is secure and VERY fast.   My tests show it can be over 150x faster then /dev/urandom.   It should compile and install on any Linux 3.10+ kernel.  It passes all the randomness tests using the dieharder tests.

srandom was created as a performance improvement to the built-in PRNG /dev/urandom number generator.  I wanted a much faster random number generator to wipe ssd disks.  Through many hours of testing, I came up with an algorithm that is many times faster than urandom, but still produces excellent random numbers that is indistinguishable from true random numbers.  You can wipe multiple SSDs at the same time.

The built-in generators (/dev/random and /dev/urandom) are technically not flawed.  /dev/random (the true random number generator) is BLOCKED most of the time waiting for more entropy makeing it basically unusable.  The PRNG /dev/urandom is unblocked, but is very slow when compared to srandom. 

What is the most important part of random numbers?  Unpredictability!  Generating random numbers which is indistinguishable from true randomness is the goal here...   There are MANY arguments against using a PRNG and some will even argue that true randomness doesn't exist.   Personally, I don't get the point.  

What makes srandom a great PRNG generator?  It includes all these features to make it's generator produce the most unpredictable/random numbers.
  * It uses two separate and different 64bit PRNGs.
  * There are two different algorithms to XOR the the 64bit PSRNGs together.
  * srandom seeds and re-seeds the three separate seeds using nano timer.
  * The module seeds the PSRNGs twice on module init.
  * It uses 16 x 512byte buffers and outputs them randomly.
  * There is a separate kernel thread that constantly updates the buffers and seeds.
  * srandom throws away a small amount of data.

The best part of srandom is it's efficiency and very high speed...  I tested many PSRNGs and found two that worked very fast and had a good distribution of numbers.  Two or three 64bit numbers are XORed.  The results is unpredictable and very high speed generation of numbers.


Why do I need this?
-------------------

The best use-case is disk wiping.  However you could use srandom to provide your applications with the fastest source of random numbers.  Why would you want to block your applications while waiting for random numbers?  Run "lsof|grep random", just to see how many applications have the random number device open...  Any security type applications rely heavily on random numbers.  For example, Apache SSL (Secure Socket Level), PGP (Pretty Good Privacy), VPN (Virtual Private Networks).  All types of Encryption, Password seeds, Tokens would rely on a source of random number.  There is many examples at https://www.random.org/testimonials/.

Compile and installation
------------------------

To build & compile the kernel module.  A pre-req is "kernel-devel".  Use yum or apt to install.

    make

To load the kernel module into the running kernel (temporary).

    make load

To unload the kernel module from the running kernel.

    make unload

To install the kernel module on your system (persistent on reboot).

    make install ; make load

To uninstall the kernel module from your system.

    make uninstall


Usage
-----

You can load the kernel module temporary, or you can install the kernel module to be persistent on reboot.

  * If you want to just test the kernel module, you should run "make load".  This will load the kernel module into the running kernel and create a /dev/srandom accessible to root only.   It can be removed with "make unload".   You can monitor the load process in /var/log/messages.
  * When you run "make install", the srandom kernel module is moved to /usr/lib/modules/.../kernel/drivers/.  If you run "make load" or reboot, the kernel module will be loaded into the running kernel, but now will replace the /dev/urandom device file.  The old /dev/urandom device is renamed (keeping it's inode number).  This allows any running process that had /dev/urandom to continue running without issues. All new requests for /dev/urandom will use the srandom kernel module.
  * Once the kernel module is loaded, you can access the module information through the /proc filesystem. For example:
```
# cat /proc/srandom
-----------------------:----------------------
Device                 : /dev/srandom
Module version         : 1.40.1
Current open count     : 5
Total open count       : 3665
Total K bytes          : 55146567
-----------------------:----------------------
Author                 : Jonathan Senkerik
Website                : http://www.jintegrate.co
github                 : http://github.com/josenk/srandom
```
  * Use the /usr/bin/srandom tool to set srandom as the system PRNG, set the system back to default PRNG, or get the status.
```
# /usr/bin/srandom help

# /usr/bin/srandom status
Module loaded
srandom if functioning correctly
/dev/urandom is LINKED to /dev/srandom (system is using srandom)

```
  * To completely remove the srandom module, use "make uninstall".   Depending if there is processes accessing /dev/srandom, you may not be able to remove the module from the running kernel.   Try "make unload", if the module is busy, then a reboot is required.




Testing & performance
---------------------

A simple dd command to read from the /dev/srandom device will show performance of the generator.  The results below are typical from my system.  Of course, your performance will vary.


The "Improved" srandom number generator

```
time dd if=/dev/srandom of=/dev/null count=64k bs=64k

65536+0 records in
65536+0 records out
4294967296 bytes (4.3 GB) copied, 1.80974 s, 2.4 GB/s

real    0m1.811s
user    0m0.012s
sys     0m1.799s


```




The "Non-Blocking" urandom number generator

```
time dd if=/dev/urandom of=/dev/null count=64k bs=64k

65536+0 records in
65536+0 records out
4294967296 bytes (4.3 GB) copied, 277.96 s, 15.5 MB/s

real    4m37.961s
user    0m0.028s
sys     4m37.923s

```



The "Blocking" random number generator.  ( I pressed [CTRL-C] after 5 minutes and got 35 bytes!)  If you really NEED true random numbers, you would need to purchase a true random number device.)

```
time dd if=/dev/random of=/dev/null count=64k bs=64k
[CTRL]-C
0+35 records in
0+0 records out
0 bytes (0 B) copied, 325.303 s, 0.0 kB/s

real    5m25.306s
user    0m0.001s
sys     0m0.003s
```


Testing randomness
------------------

The most important part of the random number device file is that is produces random/unpredictable numbers.  The golden standard of testing randomness is the dieharder test suite (http://www.phy.duke.edu/~rgb/General/dieharder.php).  The dieharder tool will easily detect flawed random number generators.   After you install dieharder, use the following command to put /dev/srandom through the battery of tests.

A note about the possibility of a test showing as "WEAK"...  If a test is repeatedly "FAILED" or "WEAK", then that is a problem.   


```
dd if=/dev/srandom |dieharder -g 200 -f - -a

#=============================================================================#
#            dieharder version 3.31.1 Copyright 2003 Robert G. Brown          #
#=============================================================================#
   rng_name    |           filename             |rands/second|
stdin_input_raw|                               -|  2.83e+07  |
#=============================================================================#
        test_name   |ntup| tsamples |psamples|  p-value |Assessment
#=============================================================================#
   diehard_birthdays|   0|       100|     100|0.81047982|  PASSED
      diehard_operm5|   0|   1000000|     100|0.05511543|  PASSED
  diehard_rank_32x32|   0|     40000|     100|0.01261989|  PASSED
    diehard_rank_6x8|   0|    100000|     100|0.47316309|  PASSED
   diehard_bitstream|   0|   2097152|     100|0.74346434|  PASSED
        diehard_opso|   0|   2097152|     100|0.98820557|  PASSED
        diehard_oqso|   0|   2097152|     100|0.38452940|  PASSED
         diehard_dna|   0|   2097152|     100|0.23079735|  PASSED
diehard_count_1s_str|   0|    256000|     100|0.35819763|  PASSED
diehard_count_1s_byt|   0|    256000|     100|0.18753110|  PASSED
 diehard_parking_lot|   0|     12000|     100|0.29327315|  PASSED
    diehard_2dsphere|   2|      8000|     100|0.35160725|  PASSED
    diehard_3dsphere|   3|      4000|     100|0.49118105|  PASSED
     diehard_squeeze|   0|    100000|     100|0.33775434|  PASSED
        diehard_sums|   0|       100|     100|0.37711818|  PASSED
        diehard_runs|   0|    100000|     100|0.54179533|  PASSED
        diehard_runs|   0|    100000|     100|0.73976903|  PASSED
       diehard_craps|   0|    200000|     100|0.66525469|  PASSED
       diehard_craps|   0|    200000|     100|0.84537370|  PASSED
 marsaglia_tsang_gcd|   0|  10000000|     100|0.10708190|  PASSED
 marsaglia_tsang_gcd|   0|  10000000|     100|0.87071126|  PASSED
         sts_monobit|   1|    100000|     100|0.30011393|  PASSED
            sts_runs|   2|    100000|     100|0.91175959|  PASSED
              <<<   etc...   >>>
```



How to configure your apps
--------------------------

  If you installed the kernel module to load on reboot, then you do not need to modify any applications to use the srandom kernel module.   It will be linked to /dev/urandom, so all applications will use it automatically.   However, if you do not want to link /dev/srandom to /dev/urandom, then you can configure your applications to use whichever device you want.   Here is a few examples....

  Java:  Use the following command line argument to tell Java to use the new random device

    -Djava.security.egd=file:///dev/srandom switch

       or

    -Djava.security.egd=file:/dev/./srandom

  Java: To make the setting as default, add the following line to the configuration file. ($JAVA_HOME/jre/lib/security/java.security)

    securerandom.source=file:/dev/./srandom


  https: (Apache SSL), Configure /etc/httpd/conf.d/ssl.conf

    SSLRandomSeed startup file:/dev/srandom 512
    SSLRandomSeed connect file:/dev/srandom 512


  Postfix: Change the following line in /etc/postfix/main.cf

    tls_random_source = dev:/dev/srandom


  PHP: Change the following line in PHP config file.

    session.entropy_file = /dev/srandom


  OpenLDAP:  Change the following line in /etc/openldap/slapd.conf

    TLSRandFile /dev/srandom



Using /dev/srandom to securely wipe SSD disks.
-----------------------------------------------


*** This will DESTROY DATA!   Use with caution! ***

*** Replace /dev/sdXX with your disk device you want to wipe.


    dd if=/dev/srandom of=/dev/sdXX bs=64k


License
-------

Copyright (C) 2019 Jonathan Senkerik

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
