// 根构建脚本：仅声明插件版本，不在根项目直接应用
plugins {
    id("com.android.application") version "8.5.2" apply false
    id("org.jetbrains.kotlin.android") version "1.9.24" apply false
    id("com.google.dagger.hilt.android") version "2.51.1" apply false
    // Room 用 KSP（避免 kapt 对 @Update 等注解的 stub 解析 bug；Hilt 仍用 kapt，两者可共存）
    id("com.google.devtools.ksp") version "1.9.24-1.0.20" apply false
}
