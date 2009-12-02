function [x,y] = pcie(bpm,num);

fid=fopen('myfile.bin','r');
dat=fread(fid,512 * num,'int');
fclose(fid);

dat = reshape(dat,2,[]);
x = dat(1,:);
y = dat(2,:);

x = reshape(x,256,[]);
y = reshape(y,256,[]);

plot(diff(x((bpm+1),:)));
