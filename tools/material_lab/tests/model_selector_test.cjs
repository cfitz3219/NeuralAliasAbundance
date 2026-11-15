// Runs the shipped viewer controller with a fake DOM/GL. This tests selection,
// not WebGL pixel quality. Pass a generated Material-Lab.html to test real data.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const template = fs.readFileSync(process.argv[2] || path.join(__dirname,'../viewer.html.in'),'utf8');
const roles = ['base_color','normal','roughness'];
function candidate(name) {
  return {label:name, description:name, dimensions:{base_color:[256,256]}, bytes:42,
    maps:Object.fromEntries(roles.map(role => [role,[name+'-'+role]]))};
}
const example = Array.from({length:2},(_,i)=>({name:'example_'+i,family:'wood',split:'development',
  original:candidate('original-'+i),coordinated:candidate('coordinated-'+i),
  comparisons:Object.fromEntries(['pbrify','hat-fidelity','hat-sharper','baseline','conventional'].map(id=>
    [id,{...candidate(id+'-'+i),maps:{base_color:[id+'-'+i]}}]))}));
const embedded = template.match(/<script id="data" type="application\/json">(.*?)<\/script>/s)[1];
const data = embedded==='__MATERIAL_DATA__'?example:JSON.parse(embedded);
const script = [...template.matchAll(/<script>(.*?)<\/script>/sg)][0][1];
function fakeGL(){
  let texture;
  const uniforms={};
  return new Proxy({
    uniforms,getUniformLocation:(_program,name)=>name,
    uniform1f:(name,value)=>{uniforms[name]=value;},
    createTexture:()=>({images:[]}),bindTexture:(_target,value)=>{texture=value;},
    texImage2D:(...args)=>texture.images.push(args.at(-1).src),
    getShaderParameter:()=>true,getProgramParameter:()=>true,
  },{get:(object,key)=>object[key] || (()=>{})});
}
function element(id){return {id,value:'',checked:false,children:[],dataset:{},textContent:'',
  classList:{toggle(){}},appendChild(child){this.children.push(child);if(!this.value)this.value=child.value;},
  replaceChildren(){this.children=[];this.value='';},get options(){return this.children;},
  getContext(){return fakeGL();}};}
const elements = new Map();
const get=id=>{if(!elements.has(id))elements.set(id,element(id));return elements.get(id);};
get('data').textContent=JSON.stringify(data);
get('compare').value='all';get('angle').value='-18';get('distance').value='2.3';
get('animate').checked=false;
get('surface-strength').value='65';
const document = {getElementById:get,createElement:element,body:element('body'),
  querySelectorAll:()=>get('materials').children};
class Image {set src(value){this._src=value;queueMicrotask(()=>this.onload());}get src(){return this._src;}}
const context=vm.createContext({document,Image,requestAnimationFrame(){},console,devicePixelRatio:1});
vm.runInContext(script+'\nglobalThis.inspect=()=>({states,current,lightTime});globalThis.select=show;globalThis.drawFrame=frame;',context);
async function main(){
  // Finish initial asynchronous texture uploads.
  await new Promise(resolve=>setImmediate(resolve));
  assert.equal(document.body.dataset.state,'ready',get('error').textContent);
  const count=Object.keys(data[0].comparisons).length;
  assert.equal(get('baseline-choice').options.length,count);
  const initial=context.inspect();
  const original=initial.states.original.bound.base_color;
  const normals=initial.states.baseline.bound.normal;
  const textures=new Set();
  for(const key of Object.keys(data[0].comparisons)){
    get('baseline-choice').value=key;
    await get('baseline-choice').onchange();
    const state=context.inspect();
    const chosen=state.states.baseline.bound.base_color;
    assert.equal(chosen.images[0],data[0].comparisons[key].maps.base_color[0]);
    textures.add(chosen);
    assert.equal(state.states.original.bound.base_color,original);
    assert.equal(state.states.baseline.bound.normal,normals);
    assert.equal(state.lightTime,0);assert.equal(get('angle').value,'-18');
    assert.equal(get('distance').value,'2.3');
    assert.equal(get('middle-title').textContent,data[0].comparisons[key].label);
  }
  assert.equal(textures.size,count);
  if(data[0].combined){
    assert.equal(count,1);
    assert.equal(get('right-title').textContent,'Combined result');
    assert.equal(get('ai-control').hidden,true);
    const state=context.inspect();
    assert.equal(state.states.coordinated.bound.base_color.images[0],state.states.baseline.bound.base_color.images[0]);
    assert.equal(state.states.coordinated.bound.authored_normal.images[0],state.states.baseline.bound.normal.images[0]);
    get('surface-strength').value='0';get('surface-strength').oninput();
    context.drawFrame(0);
    assert.equal(state.states.coordinated.gl.uniforms.surfaceStrength,0);
    assert.equal(state.states.baseline.gl.uniforms.surfaceStrength,1);
    assert.equal(get('surface-value').textContent,'0%');
    get('surface-strength').value='100';context.drawFrame(0);
    assert.equal(state.states.coordinated.gl.uniforms.surfaceStrength,1);
  }
  const selected=get('baseline-choice').value;
  await context.select(1);
  assert.equal(get('baseline-choice').value,selected);
  assert.equal(context.inspect().states.baseline.bound.base_color.images[0],data[1].comparisons[selected].maps.base_color[0]);
  get('compare').value='normal';await get('compare').onchange();
  assert.match(get('middle-caption').textContent,/Colour models are hidden/);
  assert.equal(context.inspect().states.baseline.bound.base_color.images[0],data[1].original.maps.base_color[0]);
  console.log('PASS: '+count+' model selections; original/normal and camera/light preservation; material/isolation views; combined controls where present');
}
main().catch(error=>{console.error(error);process.exitCode=1;});
