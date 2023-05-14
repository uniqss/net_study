fname=$( echo "$1" | cut -d "." -f1 )

echo building $fname.cpp  "=>" ./bin/$fname

g++ -g -Wall -std=c++2a -O0 -I/home/uniqs/asio/asio-1.28.0/include/ $fname.cpp -o ./bin/$fname
