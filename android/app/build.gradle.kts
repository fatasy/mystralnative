plugins {
    id("com.android.application")
}

android {
    namespace = "com.mystral.engine"
    compileSdk = 34
    ndkVersion = "25.2.9519653"

    defaultConfig {
        applicationId = "com.mystral.engine"
        minSdk = 24  // Android 7.0 - minimum for Vulkan
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // Target modern 64-bit architectures
            // arm64-v8a: Most Android devices (ARM64)
            // x86_64: Android emulator on Intel/AMD
            abiFilters.addAll(listOf("arm64-v8a", "x86_64"))
        }

        externalNativeBuild {
            cmake {
                // CMake arguments for the Mystral native build
                arguments.addAll(listOf(
                    "-DANDROID=ON"
                ))
                // C/C++ flags
                cppFlags.add("-std=c++17")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    externalNativeBuild {
        cmake {
            // Point to the main CMakeLists.txt (parent of android directory)
            path = file("../../CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }

    buildFeatures {
        prefab = true  // Enable prefab for SDL3 AAR if we use it
    }
}

dependencies {
    // SDL3 AAR for Java bindings (SDLActivity, etc.)
    implementation(files("../../third_party/sdl3-android/SDL3-3.2.8.aar"))
}
