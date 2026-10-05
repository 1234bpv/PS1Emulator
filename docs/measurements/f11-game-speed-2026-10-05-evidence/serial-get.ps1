param([string]$Com='COM12',[string]$Remote,[string]$OutFile,[ValidateSet('BaseStream','Buffered')][string]$Method='BaseStream',[ValidateSet('None','RequestToSend')][string]$Handshake='None')
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Diagnostics;
public class SerialGetResult {
 public int expected,received;
 public long elapsedMs;
 public bool complete;
 public string error;
}
public static class SerialGet {
 static int ReadByte(SerialPort p,bool buffered) {return buffered?p.ReadByte():p.BaseStream.ReadByte();}
 static string Line(SerialPort p,bool buffered) {
  var s=new StringBuilder();
  for(int i=0;i<1024;i++){int c=ReadByte(p,buffered);if(c<0)throw new EndOfStreamException();if(c==10)return s.ToString().TrimEnd('\r');s.Append((char)c);}
  throw new IOException("Header too long");
 }
 public static SerialGetResult Get(string com,string remote,string file,bool buffered,Handshake handshake) {
  var r=new SerialGetResult();var timer=Stopwatch.StartNew();byte[] data=null;
  using(var port=new SerialPort(com,115200,Parity.None,8,StopBits.One)) {
   port.ReadBufferSize=1048576;port.ReadTimeout=8000;port.WriteTimeout=10000;port.Handshake=handshake;
   try {
    port.Open();port.BaseStream.ReadTimeout=8000;port.DiscardInBuffer();port.DiscardOutBuffer();
    byte[] request=Encoding.ASCII.GetBytes("GET "+remote+"\n");
    if(buffered)port.Write(request,0,request.Length);else port.BaseStream.Write(request,0,request.Length);
    string header=Line(port,buffered);Console.WriteLine("[serial-get] "+header);
    if(!header.StartsWith("OK "))throw new IOException(header);
    r.expected=int.Parse(header.Substring(3));if(r.expected<0||r.expected>33554432)throw new IOException("Unexpected file size");
    data=new byte[r.expected];int nextProgress=262144;
    while(r.received<r.expected) {
     int count=Math.Min(8192,r.expected-r.received);
     int got=buffered?port.Read(data,r.received,count):port.BaseStream.Read(data,r.received,count);
     if(got<=0)throw new EndOfStreamException();r.received+=got;
     if(r.received>=nextProgress){Console.WriteLine("[serial-get] "+r.received+"/"+r.expected);nextProgress+=262144;}
    }
    File.WriteAllBytes(file,data);r.complete=true;
    byte[] bye=Encoding.ASCII.GetBytes("BYE\n");port.Write(bye,0,bye.Length);
   } catch(Exception e) {
    r.error=e.GetType().Name+": "+e.Message;
    if(data!=null){using(var f=File.Create(file+".partial")){f.Write(data,0,r.received);}}
   } finally {if(port.IsOpen)port.Close();r.elapsedMs=timer.ElapsedMilliseconds;}
  }
  return r;
 }
}
'@
$path=[IO.Path]::GetFullPath($OutFile)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
$result=[SerialGet]::Get($Com,$Remote,$path,($Method -eq 'Buffered'),[IO.Ports.Handshake]::$Handshake)
$result | Add-Member NoteProperty method $Method
$result | Add-Member NoteProperty handshake $Handshake
$result | ConvertTo-Json | Set-Content ($path+'.transfer.json') -Encoding utf8
$result | ConvertTo-Json -Compress | Write-Output
if(-not $result.complete){exit 1}
Get-FileHash -LiteralPath $path -Algorithm SHA256 | Select-Object Hash,Path | Format-List
