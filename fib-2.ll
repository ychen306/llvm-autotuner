; ModuleID = 'fib-2.bc'
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@.str = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

; Function Attrs: nounwind uwtable
define i32 @get(i32 %n) #0 {
  %1 = alloca i32, align 4
  store i32 %n, i32* %1, align 4
  %2 = load i32, i32* %1, align 4
  ret i32 %2
}

; Function Attrs: nounwind uwtable
define i32 @main() #0 {
  %1 = alloca i32, align 4
  %a = alloca i32, align 4
  %b = alloca i32, align 4
  %i = alloca i32, align 4
  %t = alloca i32, align 4
  store i32 0, i32* %1
  store i32 0, i32* %a, align 4
  store i32 1, i32* %b, align 4
  store i32 0, i32* %i, align 4
  br label %codeRepl

codeRepl:                                         ; preds = %0
  call void @main_(i32* %i, i32* %a, i32* %b, i32* %t)
  br label %2

; <label>:2                                       ; preds = %codeRepl
  %3 = load i32, i32* %a, align 4
  %4 = call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @.str, i32 0, i32 0), i32 %3)
  %5 = load i32, i32* %1
  ret i32 %5
}

declare i32 @printf(i8*, ...) #1

; Function Attrs: nounwind
define internal void @main_(i32* %i, i32* %a, i32* %b, i32* %t) #2 {
newFuncRoot:
  %0 = alloca i32, align 4
  %1 = alloca i32, align 4
  %2 = alloca i32, align 4
  %3 = alloca i32, align 4
  br label %4

.exitStub:                                        ; preds = %4
  ret void

; <label>:4                                       ; preds = %newFuncRoot, %25
  %5 = load i32, i32* %i, align 4
  %6 = icmp slt i32 %5, 40
  br i1 %6, label %7, label %.exitStub

; <label>:7                                       ; preds = %4
  %8 = load i32, i32* %a, align 4
  %9 = bitcast i32* %3 to i8*
  call void @llvm.lifetime.start(i64 4, i8* %9)
  store i32 %8, i32* %3, align 4
  %10 = load i32, i32* %3, align 4
  %11 = bitcast i32* %3 to i8*
  call void @llvm.lifetime.end(i64 4, i8* %11)
  %12 = load i32, i32* %b, align 4
  %13 = bitcast i32* %2 to i8*
  call void @llvm.lifetime.start(i64 4, i8* %13)
  store i32 %12, i32* %2, align 4
  %14 = load i32, i32* %2, align 4
  %15 = bitcast i32* %2 to i8*
  call void @llvm.lifetime.end(i64 4, i8* %15)
  %16 = add nsw i32 %10, %14
  store i32 %16, i32* %t, align 4
  %17 = load i32, i32* %b, align 4
  %18 = bitcast i32* %1 to i8*
  call void @llvm.lifetime.start(i64 4, i8* %18)
  store i32 %17, i32* %1, align 4
  %19 = load i32, i32* %1, align 4
  %20 = bitcast i32* %1 to i8*
  call void @llvm.lifetime.end(i64 4, i8* %20)
  store i32 %19, i32* %a, align 4
  %21 = load i32, i32* %t, align 4
  %22 = bitcast i32* %0 to i8*
  call void @llvm.lifetime.start(i64 4, i8* %22)
  store i32 %21, i32* %0, align 4
  %23 = load i32, i32* %0, align 4
  %24 = bitcast i32* %0 to i8*
  call void @llvm.lifetime.end(i64 4, i8* %24)
  store i32 %23, i32* %b, align 4
  br label %25

; <label>:25                                      ; preds = %7
  %26 = load i32, i32* %i, align 4
  %27 = add nsw i32 %26, 1
  store i32 %27, i32* %i, align 4
  br label %4
}

; Function Attrs: nounwind
declare void @llvm.lifetime.start(i64, i8* nocapture) #2

; Function Attrs: nounwind
declare void @llvm.lifetime.end(i64, i8* nocapture) #2

attributes #0 = { nounwind uwtable "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="true" "no-frame-pointer-elim-non-leaf" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+sse,+sse2" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="true" "no-frame-pointer-elim-non-leaf" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+sse,+sse2" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #2 = { nounwind }

!llvm.ident = !{!0}

!0 = !{!"clang version 3.7.0 (tags/RELEASE_370/final)"}
