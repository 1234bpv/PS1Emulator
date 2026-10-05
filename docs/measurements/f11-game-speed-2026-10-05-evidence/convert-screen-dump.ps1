param([string]$InputFile,[string]$OutputFile)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$raw=[IO.File]::ReadAllBytes($InputFile)
$nl=[Array]::IndexOf($raw,[byte]10)
$header=[Text.Encoding]::ASCII.GetString($raw,0,$nl)
if($header -notmatch '^PS1SCR (\d+) (\d+) (\d+) (\d+) '){throw 'screen header invalid'}
$width=[int]$Matches[1]; $height=[int]$Matches[2]; $pixel=[int]$Matches[4]
if($pixel -notin 2,4 -or $raw.Length-$nl-1 -ne $width*$height*$pixel){throw 'screen payload invalid'}
$offset=if($pixel -eq 2){66}else{54}
$mem=New-Object IO.MemoryStream
$writer=New-Object IO.BinaryWriter($mem)
$writer.Write([byte]66); $writer.Write([byte]77)
$writer.Write([int]($offset+$width*$height*$pixel)); $writer.Write([int]0); $writer.Write([int]$offset)
$writer.Write([int]40); $writer.Write([int]$width); $writer.Write([int](-$height))
$writer.Write([int16]1); $writer.Write([int16]($pixel*8)); $writer.Write([int]$(if($pixel -eq 2){3}else{0}))
$writer.Write([int]($width*$height*$pixel)); 1..4 | ForEach-Object {$writer.Write([int]0)}
if($pixel -eq 2){$writer.Write([int]0xf800);$writer.Write([int]0x07e0);$writer.Write([int]0x001f)}
$writer.Write($raw,$nl+1,$raw.Length-$nl-1); $writer.Flush(); $mem.Position=0
$bitmap=[Drawing.Bitmap]::FromStream($mem)
try {$bitmap.Save($OutputFile,[Drawing.Imaging.ImageFormat]::Png)} finally {$bitmap.Dispose();$writer.Dispose();$mem.Dispose()}
