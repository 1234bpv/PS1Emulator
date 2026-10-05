using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
public sealed class C5RfcommStream:Stream {
 [StructLayout(LayoutKind.Sequential,Pack=1)] struct BthAddress {public ushort family;public ulong address;public Guid service;public uint port;}
 [DllImport("ws2_32.dll")] static extern int WSAStartup(ushort version,IntPtr data);
 [DllImport("ws2_32.dll")] static extern int WSACleanup();
 [DllImport("ws2_32.dll")] static extern int WSAGetLastError();
 [DllImport("ws2_32.dll")] static extern IntPtr socket(int family,int type,int protocol);
 [DllImport("ws2_32.dll")] static extern int connect(IntPtr socket,ref BthAddress address,int size);
 [DllImport("ws2_32.dll")] static extern int recv(IntPtr socket,byte[] buffer,int count,int flags);
 [DllImport("ws2_32.dll")] static extern int send(IntPtr socket,byte[] buffer,int count,int flags);
 [DllImport("ws2_32.dll")] static extern int setsockopt(IntPtr socket,int level,int option,ref int value,int size);
 [DllImport("ws2_32.dll")] static extern int closesocket(IntPtr socket);
 IntPtr handle=new IntPtr(-1);bool started;int readTimeout=8000,writeTimeout=120000;
 static IOException Error(string operation){return new IOException(operation+": WSA "+WSAGetLastError());}
 public static C5RfcommStream Open(uint channel){var s=new C5RfcommStream();s.Initialize(channel);return s;}
 void Initialize(uint channel) {
  IntPtr data=Marshal.AllocHGlobal(512);
  try {int r=WSAStartup(0x202,data);if(r!=0)throw new IOException("WSAStartup "+r);started=true;}finally{Marshal.FreeHGlobal(data);}
  try {
   handle=socket(32,1,3);if(handle.ToInt64()==-1)throw Error("Bluetooth socket");
   int buffer=1048576;setsockopt(handle,0xffff,0x1002,ref buffer,4);
   ReadTimeout=readTimeout;WriteTimeout=writeTimeout;
   var address=new BthAddress{family=32,address=0xA8448113EA78UL,service=Guid.Empty,port=channel};
   if(connect(handle,ref address,Marshal.SizeOf(typeof(BthAddress)))!=0)throw Error("Bluetooth connect C5");
  }catch{Dispose(true);throw;}
 }
 public Stream BaseStream {get{return this;}}
 public override bool CanRead {get{return true;}}
 public override bool CanWrite {get{return true;}}
 public override bool CanSeek {get{return false;}}
 public override bool CanTimeout {get{return true;}}
 public override long Length {get{throw new NotSupportedException();}}
 public override long Position {get{throw new NotSupportedException();}set{throw new NotSupportedException();}}
 public override int ReadTimeout {get{return readTimeout;}set{int ms=value<0?0:value;if(setsockopt(handle,0xffff,0x1006,ref ms,4)!=0)throw Error("Receive timeout");readTimeout=value;}}
 public override int WriteTimeout {get{return writeTimeout;}set{int ms=value<0?0:value;if(setsockopt(handle,0xffff,0x1005,ref ms,4)!=0)throw Error("Send timeout");writeTimeout=value;}}
 public override int Read(byte[] buffer,int offset,int count) {
  byte[] block=new byte[count];int n=recv(handle,block,count,0);if(n<0)throw Error("Bluetooth recv");if(n>0)Buffer.BlockCopy(block,0,buffer,offset,n);return n;
 }
 public override int ReadByte(){byte[] b=new byte[1];int n=recv(handle,b,1,0);if(n<0)throw Error("Bluetooth recv byte");return n==0?-1:b[0];}
 public override void Write(byte[] buffer,int offset,int count) {
  int sent=0;while(sent<count){byte[] block=new byte[count-sent];Buffer.BlockCopy(buffer,offset+sent,block,0,block.Length);int n=send(handle,block,block.Length,0);if(n<=0)throw Error("Bluetooth send");sent+=n;}
 }
 public override void Flush(){}
 public override long Seek(long offset,SeekOrigin origin){throw new NotSupportedException();}
 public override void SetLength(long length){throw new NotSupportedException();}
 protected override void Dispose(bool disposing){if(handle.ToInt64()!=-1){closesocket(handle);handle=new IntPtr(-1);}if(started){WSACleanup();started=false;}base.Dispose(disposing);}
}
