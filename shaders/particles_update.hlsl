cbuffer SceneCB : register(b0)
{
    float4x4 ViewProj;
    float4   TimeVec;
    float4   LightDir;
    float4   LightColor;
    float4   CamPos;
    float4x4 ShadowViewProj;
    float4x4 InvViewProj;
};

cbuffer ParticleCB:register(b1) { float3 Gravity; float EmitStrength; float3 Emitter; float ParticleLife; };
struct Particle { float3 pos; float life; float3 vel; float seed; }; RWStructuredBuffer<Particle> Particles:register(u0);
float hash(float n){return frac(sin(n)*43758.5453);} float3 rand3(float n){return float3(hash(n+1.7),hash(n+11.3),hash(n+23.1));}
[numthreads(256,1,1)] void CSMain(uint3 did:SV_DispatchThreadID){uint count,stride;Particles.GetDimensions(count,stride);if(did.x>=count)return;Particle p=Particles[did.x];float lifeMax=ParticleLife>0.001?ParticleLife:4.0;float dt=0.016;float seed=float(did.x)*13.17+TimeVec.x*0.37;if(p.life<=0.001||p.life>lifeMax+0.1){float3 r=rand3(seed)*2-1;float a=hash(seed+4.0)*6.2831853;float radius=sqrt(hash(seed+8.0))*0.22;float3 emit=dot(Emitter,Emitter)>0.001?Emitter:float3(0,-0.85,0);p.pos=emit+float3(cos(a)*radius,0,sin(a)*radius);float speed=EmitStrength>0.001?EmitStrength:2.5;p.vel=normalize(float3(r.x*0.75,1.4+abs(r.y),r.z*0.75))*speed;p.life=lifeMax*(0.55+0.45*hash(seed+2.0));p.seed=seed;}else{float3 swirl=float3(sin(TimeVec.x+p.seed),0,cos(TimeVec.x*0.7+p.seed))*0.22;p.vel+=(Gravity+swirl)*dt;p.pos+=p.vel*dt;p.life-=dt;}Particles[did.x]=p;}
