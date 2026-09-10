NO-LINUX BUILD

This project is ready to build in GitHub Actions. You do NOT need Linux on your Chromebook.

1. Create a GitHub repository.
2. Upload all files/folders in this ZIP to the repository.
3. GitHub will automatically run the workflow.
4. Open the repository's Actions tab.
5. Open "Build Epson Universal Power" and the latest successful run.
6. Scroll to Artifacts and download Epson-Universal-Power-Momentum-d3f89dfe.
7. Inside the downloaded artifact is the .fap file.
8. Use qFlipper File Manager to copy the .fap to the Flipper SD card under:
   /apps/Infrared/

The workflow uses the exact Momentum SDK for commit d3f89dfe.
