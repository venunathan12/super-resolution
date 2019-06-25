import subprocess

Comm = "bin/SuperResolution"
Comm += " --upsampling_scale=2"
Comm += " --regularizer=tv"
Comm += " --regularization_parameter=0.01"
Comm += " --solver=cg"
Comm += " --solver_iterations=50"
Comm += " --optimization_iterations=20"
Comm += " --data_path=../Runner/Imgs"
Comm += " --evaluators=psnr,ssim"
Comm += " --interpolate_color"
Comm += " --verbose"
Comm += " --result_path=../Runner/OutPut/OutPut.jpg"
# Comm += " --solve_in_wavelet_domain"
# Comm += " --use_numerical_differentiation"
subprocess.call(Comm.split(' '))