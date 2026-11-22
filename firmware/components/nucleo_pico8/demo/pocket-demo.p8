pico-8 cartridge // http://www.pico-8.com
version 42
__lua__
-- FABLE-8 Pocket: original test game, standard Lua syntax; no external assets.
-- Move with E/S/A/D or arrow legends. J/Z = O, K/X = X. Collect the yellow stars.
function _init()
 x=60 y=65 score=0 frames=0
 starx=25 stary=45
end

function _update()
 if btn(0) then x=x-2 end
 if btn(1) then x=x+2 end
 if btn(2) then y=y-2 end
 if btn(3) then y=y+2 end
 x=mid(5,x,122) y=mid(31,y,121)
 if abs(x-starx)<7 and abs(y-stary)<7 then
  score=score+1
  starx=10+flr(rnd(108)) stary=36+flr(rnd(80))
 end
 if btnp(5) then score=0 end
 frames=frames+1
end

function _draw()
 cls(1)
 for i=0,15 do rectfill(i*8,0,i*8+7,5,i) end
 print("FABLE 8 POCKET",5,10,7)
 print("STARS "..score,5,19,10)
 line(0,27,127,27,13)
 rect(1,29,126,126,13)
 local pulse=2+flr((sin(frames/30)+1)*2)
 circ(starx,stary,pulse,10)
 line(starx-3,stary,starx+3,stary,10)
 line(starx,stary-3,starx,stary+3,10)
 local c=12
 if btn(4) then c=8 end
 rectfill(x-3,y-3,x+3,y+3,c)
 pset(x-1,y-1,7) pset(x+1,y-1,7)
end
