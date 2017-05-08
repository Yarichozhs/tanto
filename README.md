# tanto
Tanto - Object Based Filesystem storing objects in KV stores (Redis/MySQL/etc) or other repositories (say gmail)

Tanto is an object based filesystem where the filesystem metadata and user contents
will be stored as objects in the supported backends. It is a FUSE based filesystem
and the supported backends include key values stores such as redis or mySQL. In the
future, it will include other backends such as mail servers (say IMAP/gmail). 

Backend storage can be in local host or in remote host with network information 
configured during mount time. 

For now, it is prototyped using redis key value store. 

1.  Build 

cd <tanto source file location>
make tanto

2. Start the redis server ( prototype for now, start the redis server in local host. Will update remote support in next version )

3. Create a mount point 

mkdir /tmp/tanto_root

4. Mount the tanto file system

./tanto -f -s /tmp/tanto_root

5. Access the file system as any other file system

bash $ cd /tmp/tanto_root

user1:/tmp/tanto_root$ ls
user1:/tmp/tanto_root$ touch test1

user1:/tmp/tanto_root$ cat > test2
Testing tanto file system
It is file 2

user1:/tmp/tanto_root$ echo "Testing another file" > test3

user1:/tmp/tanto_root$ ls
test1  test2  test3
user1:/tmp/tanto_root$ ls -ldtr *
-rw-rw-r-- 1 naaaag naaaag 4096 Dec 31  1969 test3
-rw-rw-r-- 1 naaaag naaaag 4096 Dec 31  1969 test2
-rw-rw-r-- 1 naaaag naaaag    0 Dec 31  1969 test1

user1:/tmp/tanto_root$ cat test2
Testing tanto file system
It is file 2
user1:/tmp/tanto_root$ cat test3
Testing another file

user1:/tmp/tanto_root$ mkdir dir1
user1:/tmp/tanto_root$ cd dir1

user1:/tmp/tanto_root/dir1$ echo "testing one more file " > test1
user1:/tmp/tanto_root/dir1$ cat test1
testing one more file 

user1:/tmp/tanto_root/dir1$ ls -ldtr *
-rw-rw-r-- 1 naaaag naaaag 4096 Dec 31  1969 test1
user1:/tmp/tanto_root/dir1$ cd ..
user1:/tmp/tanto_root$ ls -ldtr *
-rw-rw-r-- 1 naaaag naaaag 4096 Dec 31  1969 test3
-rw-rw-r-- 1 naaaag naaaag 4096 Dec 31  1969 test2
-rw-rw-r-- 1 naaaag naaaag    0 Dec 31  1969 test1
drwxrwxr-x 1 naaaag naaaag 4096 Dec 31  1969 dir1


