/* Test the actual packed joystick page against a local fake Node 5 HTTP server. */
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),http=require('node:http')
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright')
const root=path.resolve(__dirname,'../../node5_dash/data'),commands=[]
let fail=false,lastAt=0,seq=0,replyDelay=0
const state={ready:true,held:false,token:10,reverse:false,remoteMode:true,throttlePwm:0,steeringDirection:0,steeringPwm:0,angleDeg:null,appliedThrottlePwm:0,powertrainReason:0,message:''}
const reset=()=>{state.held=false;state.message='';state.throttlePwm=state.steeringPwm=state.steeringDirection=state.appliedThrottlePwm=0;++state.token;seq=0}
const hornHttp=require('./horn-http.cjs')(()=>state.remoteMode)
const server=http.createServer((req,res)=>{
  if(hornHttp.handle(req,res))return
  const pathname=new URL(req.url,'http://localhost').pathname
  if(pathname==='/api/joystick'){
    if(state.held&&Date.now()-lastAt>=350)reset()
    if(req.method==='POST'){
      let text='';req.on('data',c=>text+=c);req.on('end',()=>{
        const p=new URLSearchParams(text),c=Object.fromEntries(p);commands.push(c)
        let status=200
        if(c.action==='stop')reset()
        else if(fail)status=503
        else if(Number(c.token)!==state.token||Number(c.seq)<=seq)status=409
        else{seq=Number(c.seq);lastAt=Date.now();state.held=true;state.throttlePwm=state.appliedThrottlePwm=Number(c.throttle);state.steeringDirection=Number(c.direction);state.steeringPwm=Number(c.steering);state.reverse=c.reverse==='1';state.message='REMOTE ACTIVE / PHYSICAL PEDAL BLOCKED'}
        const body=JSON.stringify(state),delay=replyDelay;replyDelay=0
        setTimeout(()=>{res.writeHead(status,{'Content-Type':'application/json'});res.end(body)},delay)
      });return
    }
    res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify(state));return
  }
  const target=path.resolve(root,['/control','/control/'].includes(pathname)?'index.html':pathname.slice(1)),gz=fs.existsSync(target+'.gz'),file=gz?target+'.gz':target
  if(!target.startsWith(root+path.sep)||!fs.existsSync(file)){res.writeHead(404);res.end();return}
  const mime={'.html':'text/html','.js':'application/javascript','.css':'text/css','.png':'image/png','.svg':'image/svg+xml'}
  res.writeHead(200,{'Content-Type':mime[path.extname(target)]||'application/octet-stream',...(gz?{'Content-Encoding':'gzip'}:{})});fs.createReadStream(file).pipe(res)
})
;(async()=>{
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve))
  const browser=await chromium.launch({headless:true,executablePath:process.env.BROWSER_PATH,args:['--no-sandbox']})
  try{
    const context=await browser.newContext({viewport:{width:1280,height:800},hasTouch:true}),page=await context.newPage(),errors=[]
    page.on('pageerror',e=>errors.push(e.message))
    await page.goto(`http://127.0.0.1:${server.address().port}/control`)
    const pad=page.getByRole('application',{name:'Drive joystick'})
    await page.waitForFunction(()=>document.querySelector('.joystick-pad')?.getAttribute('aria-disabled')==='false')
    const horn=page.getByRole('button',{name:'Hold horn to sound'})
    const hornResponse=action=>page.waitForResponse(r=>r.url().endsWith('/api/horn')&&r.request().method()==='POST'&&new URLSearchParams(r.request().postData()).get('action')===action)
    await page.waitForFunction(()=>!document.querySelector('.horn-hold')?.disabled)
    await horn.scrollIntoViewIfNeeded();let hb=await horn.boundingBox()
    let hornEvent=hornResponse('press');await page.mouse.move(hb.x+hb.width/2,hb.y+hb.height/2);await page.mouse.down();await hornEvent
    assert.equal(hornHttp.state.active,true);await hornResponse('press')
    hornEvent=hornResponse('stop');await page.mouse.up();await hornEvent;assert.equal(hornHttp.state.active,false)
    await page.waitForFunction(()=>!document.querySelector('.horn-hold')?.disabled)
    hornEvent=hornResponse('press');await horn.focus();await page.keyboard.down('Enter');await hornEvent
    hornEvent=hornResponse('stop');await page.evaluate(()=>window.dispatchEvent(new Event('blur')));await hornEvent;await page.keyboard.up('Enter');assert.equal(hornHttp.state.active,false)
    await page.waitForFunction(()=>!document.querySelector('.horn-hold')?.disabled)
    hornEvent=hornResponse('press');await horn.focus();await page.keyboard.down(' ');await hornEvent
    hornEvent=hornResponse('stop');await page.getByRole('button',{name:'STOP',exact:true}).dispatchEvent('pointerdown');await hornEvent;await page.keyboard.up(' ');assert.equal(hornHttp.state.active,false)
    await page.evaluate(()=>scrollTo(0,0))
    const response=action=>page.waitForResponse(r=>r.url().endsWith('/api/joystick')&&r.request().method()==='POST'&&new URLSearchParams(r.request().postData()).get('action')===action)
    const values=async(t,s)=>page.waitForFunction(([t,s])=>document.querySelector('[data-testid=throttle-command]')?.textContent===String(t)&&document.querySelector('[data-testid=steering-command]')?.textContent===String(s),[t,s])
    let b=await pad.boundingBox(),cx=b.x+b.width/2,cy=b.y+b.height/2,r=(b.width-64)/2
    await page.waitForFunction(()=>!document.querySelector('.horn-hold')?.disabled)
    hb=await horn.boundingBox()
    const hornTouch=await context.newCDPSession(page),hp={id:71,x:hb.x+hb.width/2,y:hb.y+hb.height/2},jp={id:72,x:cx,y:cy-r}
    hornEvent=hornResponse('press');await hornTouch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[hp]});await hornEvent
    let simultaneous=response('move');await hornTouch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[hp,jp]});await simultaneous
    assert.equal(hornHttp.state.active,true);assert.equal(state.held,true)
    simultaneous=response('stop');await hornTouch.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[jp]});await simultaneous
    assert.equal(hornHttp.state.active,true,'Releasing joystick preserves held horn')
    hornEvent=hornResponse('stop');await hornTouch.send('Input.dispatchTouchEvent',{type:'touchCancel',touchPoints:[]});await hornEvent;assert.equal(hornHttp.state.active,false)
    await page.waitForFunction(()=>!document.querySelector('.horn-hold')?.disabled)
    hb=await horn.boundingBox();hornEvent=hornResponse('press');await page.mouse.move(hb.x+hb.width/2,hb.y+hb.height/2);await page.mouse.down();await hornEvent
    hornEvent=hornResponse('stop');await page.mouse.move(1,1);await hornEvent;await page.mouse.up();assert.equal(hornHttp.state.active,false)
    let event=response('move');await page.mouse.move(cx,cy);await page.mouse.down();await event
    await page.mouse.move(cx+r,cy-r);await values(180,180);await response('move')
    assert.equal(commands.at(-1).throttle,'180');assert.equal(commands.at(-1).steering,'180');assert.equal(commands.at(-1).direction,'2')
    await page.screenshot({path:'/tmp/srt-joystick-desktop.png',animations:'disabled'})
    // Export the actual rendered UI for the design canvas, with local CSS inlined.
    const css=await page.locator('link[rel=stylesheet]').evaluateAll(async links=>(await Promise.all(links.map(l=>fetch(l.href).then(r=>r.text())))).join('\n'))
    const html=await page.evaluate(css=>{const doc=document.documentElement.cloneNode(true);doc.querySelectorAll('script,link').forEach(n=>n.remove());const style=document.createElement('style');style.textContent=css;doc.querySelector('head').append(style);return '<!DOCTYPE html>\n'+doc.outerHTML},css)
    fs.writeFileSync(path.resolve(__dirname,'../../.superdesign/tmp/joystick-implemented.html'),html)
    await page.mouse.move(cx,cy-r);await values(255,0);await response('move');assert.equal(commands.at(-1).throttle,'255');assert.equal(commands.at(-1).steering,'0')
    await page.mouse.move(cx+r,cy);await values(0,255);await response('move');assert.equal(commands.at(-1).throttle,'0')
    await page.mouse.move(cx-r,cy+r);await values(180,180);await response('move');assert.equal(commands.at(-1).reverse,'1');assert.equal(commands.at(-1).direction,'1')
    event=response('stop');await page.mouse.up();await event;await values(0,0);assert.equal(state.held,false)
    await page.waitForFunction(()=>document.querySelector('.joystick-pad')?.getAttribute('aria-disabled')==='false')
    // Captured touch continues outside the circle even with a delayed live ACK.
    const edgeTouch=await context.newCDPSession(page)
    event=response('move');await edgeTouch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[{id:5,x:cx,y:cy}]});await event
    for(let angle=45;angle<=360;angle+=45){
      if(angle===45)replyDelay=270
      event=response('move')
      await edgeTouch.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{id:5,x:cx+1.25*r*Math.cos(angle*Math.PI/180),y:cy-1.25*r*Math.sin(angle*Math.PI/180)}]})
      await event;assert.equal(state.held,true,`Touch stays captured beyond edge ${angle}`)
      assert.ok(!(await page.locator('.joystick-status').textContent()).includes('CONNECTION LOST'))
    }
    event=response('stop');await edgeTouch.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});await event;assert.equal(state.held,false)
    await pad.focus();event=response('move');await page.keyboard.down('ArrowUp');await event;await page.keyboard.down('ArrowRight');await values(180,180)
    await page.keyboard.up('ArrowRight');await values(255,0);event=response('stop');await page.keyboard.up('ArrowUp');await event
    // Speed ceilings apply to the radial axis values, not the wheel angle.
    for(const [id,value] of [['throttle-limit',128],['steering-limit',60]])await page.locator('#'+id).evaluate((el,value)=>{Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set.call(el,value);el.dispatchEvent(new Event('input',{bubbles:true}))},value)
    event=response('move');await page.mouse.move(cx,cy);await page.mouse.down();await event;await page.mouse.move(cx+r,cy-r);await values(91,42)
    event=response('stop');await page.getByRole('button',{name:'STOP',exact:true}).dispatchEvent('pointerdown');await event;await page.mouse.up();await values(0,0)
    // Cancel and focus loss stop both axes; a held pointer cannot resume afterward.
    const touch=await context.newCDPSession(page),point={id:1,x:cx,y:cy-r}
    event=response('move');await touch.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[point]});await event
    event=response('stop');await touch.send('Input.dispatchTouchEvent',{type:'touchCancel',touchPoints:[]});await event
    event=response('move');await page.mouse.move(cx,cy);await page.mouse.down();await event;await page.mouse.move(cx,cy-r)
    event=response('stop');await page.evaluate(()=>window.dispatchEvent(new Event('blur')));await event;await page.mouse.up();await values(0,0)
    event=response('move');await page.mouse.move(cx,cy);await page.mouse.down();await event;fail=true;await response('stop');await page.mouse.up()
    const count=commands.length;fail=false;await page.waitForFunction(()=>document.querySelector('.joystick-pad')?.getAttribute('aria-disabled')==='false')
    assert.equal(commands.slice(count).filter(c=>c.action==='move').length,0)
    for(const [width,height] of [[1280,800],[800,480],[390,844],[320,700]]){
      await page.setViewportSize({width,height});await page.evaluate(()=>scrollTo(0,0))
      assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),'No horizontal overflow')
      const bottom=await page.locator('.joystick-stop').evaluate(el=>el.getBoundingClientRect().bottom)
      if(bottom>height)await page.screenshot({path:'/tmp/srt-joystick-overflow.png',fullPage:true})
      assert.ok(bottom<=height,`STOP remains visible at ${width}x${height} (bottom ${bottom})`)
      if(width===390)await page.screenshot({path:'/tmp/srt-joystick-mobile.png',animations:'disabled'})
    }
    assert.deepEqual(errors,[])
    console.log('Packed joystick passed: horn press/release/cancel/blur/STOP, independent simultaneous joystick + horn, touch edges/corners with 270 ms ACK, compass/diagonal PWM, continuous speed changes, keyboard, adjustable limits, release/STOP/cancel/blur, no resume after failure, phone/landscape layouts')
  }finally{await browser.close();server.close()}
})().catch(error=>{console.error(error);server.close();process.exitCode=1})
